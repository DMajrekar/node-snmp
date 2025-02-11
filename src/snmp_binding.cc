#include <assert.h>
#include <stdlib.h>

#include <ev.h>

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/pdu_api.h>

extern "C" {

// #include <net-snmp/mib_api.h>
// we  can't include  mib_api.h  where  this function  is  declared, it  causes
// collision with node namespace.
int read_objid(const char *, oid *, size_t *);

oid* snmp_parse_oid(const char * argv,
    oid * root,
    size_t * rootlen
    );

} // extern "C"

#include <node.h>
#include <node_events.h>
#include <node_buffer.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#include <sstream>
#include <vector>
#include <algorithm>
#include <deque>
#include <memory>
#include <list>

// TODO's: exception safety, RAII (see PerformRequest's handling of pdu for
// example of the WRONG way to do it). Does RAII even work v8::ThrowException?
// Plug memory leaks in error paths.


namespace js = v8;
using namespace v8;

#if 1 && MODULE_EXPORTS_DOC

  // net-snmp "single session" (threadsafe) interface information
  // ============================================================
  - session defines peer, tcp/udp, timeouts and credentials to use
  - session has at most one socket, and when querying select_info, only reports
    one timeout (least of all timeouts of all outstanding requests)
  - PDU -  one request,  asociated with  session (sessions  keeps copy,  we can
    deallocate the PDU after snmp_sess_[async]_send)
  - Multiple  PDUs can  be queued  at same  time, we  don''t need  to wait  for
    completion.
  - both   session  and   PDU  can   have  callback.   Session  callback   gets
    called  only  if  PDU  callback  was NULL  (see  snmp_api.c:5247,  deep  in
    _sess_process_packet).
  - callback  receives msgid  in it''s  parameters to  help pair  the requests.
    Msgid is autogenerated by snmp internals in snmp_pdu_create.

  // generic net-snmp behaviour and pitfalls
  // =======================================
  - GETNEXT operation doesn''t  need any "previous" context.  It requires input
    OID just like GET, but returns value  of next strictly greater OID from the
    tree. Fails  only when used at  OID at or past  the tree end. There  can be
    multiple GETNEXT queries in progress at once.
  - GET and GET_NEXT support obtaining multiple  values in one query (just call
    snmp_add_null_var more than  once). GET_BULK is only needed  to fetch whole
    subtree (and available only in SNMP2c and greater protocol versions).
  - it is  unclear what callback should  return. All examples just  return 1 in
    every case.

#endif // MODULE_EXPORTS_DOC

// ==== SnmpSessionManager {{{

class SnmpSessionManager {
  public:
    struct storage_el {
      void* snmpHandle_;
      ev_io io_watcher_;
    };

    typedef std::list<storage_el> storage_type;
    typedef storage_type::iterator storage_iterator;

    struct ex_prepare {
      ev_prepare watcher_;
      SnmpSessionManager* selfPtr_;
    };
    struct ex_check {
      ev_check watcher_;
      SnmpSessionManager* selfPtr_;
    };
    struct ex_timeout {
      bool active_;
      ev_timer watcher_;
      SnmpSessionManager* selfPtr_;

      ex_timeout() : active_(false), selfPtr_(NULL) {}
    };

  private:
    static SnmpSessionManager* defaultInst_;

    storage_type storage_;
    ex_prepare prepare_;
    ex_check check_;
    ex_timeout timeout_;
#if EV_MULTIPLICITY
    struct ev_loop* loop_;
#endif

    SnmpSessionManager() {
      prepare_.selfPtr_ = this;
      check_.selfPtr_ = this;
      timeout_.selfPtr_ = this;
#if EV_MULTIPLICITY
      loop_ = NULL;
#endif
    }

    SnmpSessionManager(const SnmpSessionManager&);
    SnmpSessionManager& operator==(const SnmpSessionManager&);

    void prepare_cb_impl(EV_P);
    void check_cb_impl(EV_P);

  public:
    ~SnmpSessionManager() {
      assert(storage_.empty());
    }

    void addClient(void* aSnmp);
    void removeClient(void* aSnmp);

    static void prepare_cb(EV_P_ ev_prepare* w, int revents);
    static void check_cb(EV_P_ ev_check* w, int revents);
    static void timeout_cb(EV_P_ ev_timer* w, int revents);

    static SnmpSessionManager* default_inst();

    // call with ev_loop_new result
    static SnmpSessionManager* create(EV_P);
};

SnmpSessionManager* SnmpSessionManager::defaultInst_ = NULL;

SnmpSessionManager* SnmpSessionManager::default_inst() {
  if (!defaultInst_) {
    defaultInst_ = new SnmpSessionManager();
#if EV_MULTIPLICITY
    defaultInst_->loop_ = ev_default_loop(0); // EV_DEFAULT;
#endif
  }
  return defaultInst_;
}

SnmpSessionManager* SnmpSessionManager::create(EV_P) {
  SnmpSessionManager* result = new SnmpSessionManager();
#if EV_MULTIPLICITY
  result->loop_ = loop;
#endif
  return result;
}

void SnmpSessionManager::timeout_cb(
    EV_P_  ev_timer* w, int revents)
{
  assert(false && "timeout callback shouldn't have been called directly!");
}

void SnmpSessionManager::prepare_cb(
    EV_P_  ev_prepare* w, int revents)
{
  ex_prepare* data = reinterpret_cast<ex_prepare*>(w);
  assert(!data->selfPtr_->storage_.empty());
  data->selfPtr_->prepare_cb_impl(EV_A);
}

void SnmpSessionManager::prepare_cb_impl(EV_P) {
  assert(!this->storage_.empty());

#ifndef NDEBUG
  // zero initialized, used for assert checks
  static unsigned int zero[sizeof(fd_set) / sizeof(unsigned int)];
#endif

  int nfds = 0;
  fd_set readSet;
  struct timeval timeout;
  int block = 1;

  FD_ZERO(&readSet);

  storage_iterator it_end = this->storage_.end();
  for(storage_iterator it = this->storage_.begin();
      it != it_end; ++it)
  {
    if (!it->snmpHandle_) {
      continue;
    }
    int retval = snmp_sess_select_info(it->snmpHandle_, &nfds, &readSet,
        &timeout, &block);
#ifdef ENABLE_DEBUG_PRINTS
    // fprintf(stderr, "snmp_sess_select_info: %x -> %d\n", readSet, nfds - 1);
#endif

#ifndef NDEBUG
    // validity of asumptions used here is NOT guaranteed by net-snmp. It could
    // in theory add any number of read  descriptors to the set. But when using
    // session API, it  happens to add only one descriptor  per session handle,
    // and it is equal to nfds - 1. We  save several loops from 0 to nfds - one
    // for each managed handle.
    assert(retval == 1);
    assert(FD_ISSET((nfds - 1), &readSet));
    FD_CLR((nfds - 1), &readSet);
    assert(!memcmp(&readSet, zero, sizeof(fd_set)));
#endif

    ev_io_set(&it->io_watcher_, nfds - 1, EV_READ);
    ev_io_start(EV_A_   &it->io_watcher_);

#ifdef ENABLE_DEBUG_PRINTS
    fprintf(stderr, "prepare: listen for read event on fd %d\n", it->io_watcher_.fd);
#endif

    nfds = 0;
  }
  if (!block) {
    ev_tstamp next_timeout = timeout.tv_usec;
    next_timeout /= 1000000;
    next_timeout += timeout.tv_sec;
#ifdef ENABLE_DEBUG_PRINTS
    fprintf(stderr, "block until %lf\n", next_timeout);
#endif
    this->timeout_.active_ = true;
    ev_timer_init(&this->timeout_.watcher_, &SnmpSessionManager::timeout_cb,
        next_timeout, 0.0);
    ev_timer_start(EV_A_   &this->timeout_.watcher_);
  }
}

void SnmpSessionManager::check_cb(
    EV_P_   ev_check* w, int revents)
{
  ex_check* data = reinterpret_cast<ex_check*>(w);
  assert(!data->selfPtr_->storage_.empty());
  data->selfPtr_->check_cb_impl(EV_A);
}

void SnmpSessionManager::check_cb_impl(EV_P) {
  fd_set readSet;
  FD_ZERO(&readSet);

  if (this->timeout_.active_) {
    ev_timer_stop(EV_A_   &this->timeout_.watcher_);
    this->timeout_.active_ = false;
  }

  storage_iterator it_end = storage_.end();
  storage_iterator it_next;
  for(storage_iterator it = storage_.begin();
      it != it_end; it = it_next)
  {
    it_next = it;
    ++it_next;
    if (!it->snmpHandle_) {
      storage_.erase(it);
      continue;
    }

    if (!it->snmpHandle_) {
      continue;
    }

    int revents = ev_clear_pending(EV_A_ &it->io_watcher_);
    if ((revents & READ) == READ) {
#ifdef ENABLE_DEBUG_PRINTS
      fprintf(stderr, "read on fd %d\n", it->io_watcher_.fd);
#endif
      FD_SET(it->io_watcher_.fd, &readSet);
      snmp_sess_read(it->snmpHandle_, &readSet);
    } else {
      snmp_sess_timeout(it->snmpHandle_);
    }
    ev_io_stop(EV_A_   &it->io_watcher_);

    if (!it->snmpHandle_) {
      storage_.erase(it);
      continue;
    }
  }

  if (storage_.empty()) {
    ev_prepare_stop(EV_A_   &this->prepare_.watcher_);
    ev_check_stop(EV_A_   &this->check_.watcher_);
  }
}

void SnmpSessionManager::addClient(void* aSnmp) {
  if (storage_.empty()) {
    ev_prepare_init(&this->prepare_.watcher_, SnmpSessionManager::prepare_cb);
    ev_check_init(&this->check_.watcher_, SnmpSessionManager::check_cb);

#if EV_MULTIPLICITY
    ev_prepare_start(this->loop_, &this->prepare_.watcher_);
    ev_check_start(this->loop_, &this->check_.watcher_);
#else
    ev_prepare_start(&this->prepare_.watcher_);
    ev_check_start(&this->check_.watcher_);
#endif
  }
  storage_.push_front((storage_el){ aSnmp });
}

namespace {
struct handleFind {
  private:
    void* handle_;

  public:
    handleFind(void* aHandle)
      : handle_(aHandle)
    { }
    bool operator()(SnmpSessionManager::storage_el& aElement) {
      return aElement.snmpHandle_ == handle_;
    }
};
}

void SnmpSessionManager::removeClient(void* aSnmp) {
  storage_iterator it = std::find_if(storage_.begin(), storage_.end(),
      handleFind(aSnmp));
  assert(it != storage_.end());

  // only check_cb_impl is allowed to remove items from storage_
  it->snmpHandle_ = NULL;
}

// }}}



enum { VT_NUMBER, VT_TEXT, VT_OID, VT_RAW, VT_NULL };

// ===== class SnmpValue {{{
class SnmpValue : public node::ObjectWrap {
  private:
    static Persistent<v8::FunctionTemplate> constructorTemplate_;

    u_char type_;
    std::vector<unsigned char> data_;

    SnmpValue() {}

  public:
    static Handle<Value> GetType(const Arguments& args);
    static Handle<Value> GetData(const Arguments& args);

    static Handle<Value> New(u_char type, void* data, std::size_t length);

    static void Initialize(Handle<Object> target);
};

Persistent<v8::FunctionTemplate> SnmpValue::constructorTemplate_;

// Handle<Value> SnmpValue::GetType(const Arguments& args) {{{
Handle<Value> SnmpValue::GetType(const Arguments& args) {
  SnmpValue* inst = ObjectWrap::Unwrap<SnmpValue>(args.This());

  int kResult = VT_RAW;
  switch (inst->type_) {
    case ASN_INTEGER:
    case ASN_GAUGE:
    case ASN_COUNTER:
    case ASN_UINTEGER:
    case ASN_TIMETICKS:
    case ASN_COUNTER64:
#ifdef NETSNMP_WITH_OPAQUE_SPECIAL_TYPES
    case ASN_OPAQUE_I64:
    case ASN_OPAQUE_U64:
    case ASN_OPAQUE_COUNTER64:
    case ASN_OPAQUE_FLOAT:
    case ASN_OPAQUE_DOUBLE:
#endif
      kResult = VT_NUMBER;
      break;
    case ASN_OCTET_STR:
      kResult = VT_TEXT;
      break;
    case ASN_OBJECT_ID:
      kResult = VT_OID;
      break;
    case ASN_NULL:
      kResult = VT_NULL;
      break;
    case ASN_BIT_STR:
    case ASN_OPAQUE:
    case ASN_IPADDRESS:
      kResult = VT_RAW;
      break;
    default:
      return v8::ThrowException(
          NODE_PSYMBOL("internal error, unexpected variable"
            "type received from net-snmp"));
  }
  return v8::Number::New(kResult);
}
// }}}

// Handle<Value> SnmpValue::GetData(const Arguments& args) {{{
Handle<Value> SnmpValue::GetData(const Arguments& args) {
  HandleScope kScope;

  SnmpValue* inst = ObjectWrap::Unwrap<SnmpValue>(args.This());

  netsnmp_vardata data; // union of pointers, it's enough to set one of
                        // them
  data.string = inst->data_.data();

  switch (inst->type_) {       // snmpwalk dumps type as this:
                               // (mib.c: snprint_variable)
    case ASN_INTEGER:          // "%ld" on *val.integer
      return kScope.Close(v8::Int32::New(*data.integer));
#ifdef NETSNMP_WITH_OPAQUE_SPECIAL_TYPES
    case ASN_OPAQUE_I64:       // custom 32bit-compatible printer for "%lld"
                               // (int64.c:printI64) on val.counter->high,low
      {
        uint64_t buf;
        buf = data.counter64->high;
        buf <<= 32;
        buf |= data.counter64->low;
        double num = *((int64_t*)buf);
        return kScope.Close(v8::Number::New(num));
      }
#endif
    case ASN_GAUGE:            // "%u" on (unsigned int)(*val.integer & 0xffffffff))
    case ASN_COUNTER:          //                  -||-
    case ASN_UINTEGER:         // "%lu" on *val.integer
    case ASN_TIMETICKS:        // %lu applied to *var->integer
      return kScope.Close(v8::Uint32::New(*((unsigned int*)data.integer)));
    case ASN_COUNTER64:        // custom 32bit-compatible printer for "%llu"
#ifdef NETSNMP_WITH_OPAQUE_SPECIAL_TYPES
    case ASN_OPAQUE_U64:       // custom 32bit-compatible printer for "%llu"
    case ASN_OPAQUE_COUNTER64: // custom 32bit-compatible printer for "%llu"
                               // (int64.c:printU64)
#endif
      {
        uint64_t buf;
        buf = data.counter64->high;
        buf <<= 32;
        buf |= data.counter64->low;
        double num = buf;
        return kScope.Close(v8::Number::New(num));
      }
#ifdef NETSNMP_WITH_OPAQUE_SPECIAL_TYPES
    case ASN_OPAQUE_FLOAT:     // %f applied to *var->floatVal
      {
        double num = *data.floatVal;
        return kScope.Close(v8::Number::New(num));
      }
    case ASN_OPAQUE_DOUBLE:    // %f applied to *var->doubleVal
                               // XXX: shouldn't it be %lf?
      {
        double num = *data.doubleVal;
        return kScope.Close(v8::Number::New(num));
      }
#endif
    case ASN_OBJECT_ID:        // when not translated by mib, use .X.Y.Z....
                               // applied to val->objid
      {
        assert((inst->data_.size() % sizeof(oid)) == 0);
        size_t end = inst->data_.size() / sizeof(oid);
        Local<Array> result = v8::Array::New(end);
        for (size_t i = 0; i < end; ++i) {
          double num = data.objid[i];
          result->Set(i, v8::Number::New(num));
        }
        return kScope.Close(result);
      }
    case ASN_OCTET_STR:        // needs "output format" param. Guess algorithm
                               // searches for !isprint character, print as hex
                               // output if one is found, "%s" on val->string
                               // otherwise
    case ASN_BIT_STR:          // haven't encountered this one yet...
    case ASN_OPAQUE:           // uchar as hex string
    case ASN_IPADDRESS:        // print 4 uchar as IPv4 address
      {
        // many thanks to
        // http://sambro.is-super-awesome.com/2011/03/03/creating-a-proper-buffer-in-a-node-c-addon/
        // for a guide how to return Buffer from a function.
        node::Buffer* slowBuffer = node::Buffer::New(inst->data_.size());
        memcpy(
            node::Buffer::Data(slowBuffer),
            inst->data_.data(),
            inst->data_.size());
        v8::Local<v8::Object> globalObj = v8::Context::GetCurrent()->Global();
        v8::Local<v8::Function> bufferConstructor =
          v8::Local<v8::Function>::Cast(
              globalObj->Get(v8::String::New("Buffer")));
        v8::Handle<v8::Value> constructorArgs[3] = {
          slowBuffer->handle_,
          v8::Integer::New(inst->data_.size()),
          v8::Integer::New(0)
        };
        v8::Local<v8::Object> result =
          bufferConstructor->NewInstance(3, constructorArgs);
        return kScope.Close(result);
      }
    case ASN_NULL:             // buffer size is 0, print as "NULL" const
                               // string
      {
        return kScope.Close(v8::Null());
      }
    default:
      return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("internal error, unexpected variable "
            "type received from net-snmp")));
  }
}
// }}}

// Handle<Value> SnmpValue::New(...) {{{
Handle<Value> SnmpValue::New(u_char type, void* data, std::size_t length) {
  HandleScope kScope;

  SnmpValue* v = new SnmpValue();
  v->type_ = type;
  v->data_.resize(length);
  memcpy(&v->data_[0], data, length);

  Local<Object> b = constructorTemplate_->GetFunction()->NewInstance(0, NULL);

  v->Wrap(b);
  return kScope.Close(b);
}
// }}}

// stolen and modified from node.h
#define SNMP_DEFINE_HIDDEN_CONSTANT(target, constant)                     \
  (target)->Set(v8::String::NewSymbol(#constant),                         \
                v8::Integer::New(constant),                               \
                static_cast<v8::PropertyAttribute>(                       \
                  v8::ReadOnly|v8::DontDelete|v8::DontEnum))

// void SnmpValue::Initialize(Handle<Object> target) {{{
void SnmpValue::Initialize(Handle<Object> target) {
  js::HandleScope kScope;

  Local<FunctionTemplate> t = FunctionTemplate::New();
  constructorTemplate_ = Persistent<FunctionTemplate>::New(t);
  constructorTemplate_->InstanceTemplate()->SetInternalFieldCount(1);
  constructorTemplate_->SetClassName(String::NewSymbol("Value"));

  // t->Inherit(EventEmitter::constructor_template);

  NODE_SET_PROTOTYPE_METHOD(t, "GetType", SnmpValue::GetType);
  NODE_SET_PROTOTYPE_METHOD(t, "GetData", SnmpValue::GetData);

  target->Set(String::NewSymbol("Value"),
      constructorTemplate_->GetFunction());

  // support x == SnmpValue.VT_NUMBER
  SNMP_DEFINE_HIDDEN_CONSTANT(t, VT_NUMBER);
  SNMP_DEFINE_HIDDEN_CONSTANT(t, VT_TEXT);
  SNMP_DEFINE_HIDDEN_CONSTANT(t, VT_OID);
  SNMP_DEFINE_HIDDEN_CONSTANT(t, VT_RAW);
  SNMP_DEFINE_HIDDEN_CONSTANT(t, VT_NULL);

  // support x == v.VT_NUMBER (v is of type Value)
  SNMP_DEFINE_HIDDEN_CONSTANT(t->InstanceTemplate(), VT_NUMBER);
  SNMP_DEFINE_HIDDEN_CONSTANT(t->InstanceTemplate(), VT_TEXT);
  SNMP_DEFINE_HIDDEN_CONSTANT(t->InstanceTemplate(), VT_OID);
  SNMP_DEFINE_HIDDEN_CONSTANT(t->InstanceTemplate(), VT_RAW);
  SNMP_DEFINE_HIDDEN_CONSTANT(t->InstanceTemplate(), VT_NULL);
}
// }}}

// }}}



// Using Array of Uint32 should be OK,  OIDs are limited to 0..2^32 range, even
// when  the  underlying type  is  8  bytes  integer  on amd64  platforms  (see
// MAX_SUBID in net-snmp types.h).
// v8::Handle<v8::Value> read_objid_wrapper(const Arguments& args) {{{
v8::Handle<v8::Value> read_objid_wrapper(const Arguments& args) {
  HandleScope kScope;

  if (args.Length() != 1) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - missing aOid")));
  }
  if (!args[0]->IsString()) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - string expected")));
  }

  oid oid[MAX_OID_LEN];
  std::size_t oidLength = MAX_OID_LEN;


  if (!read_objid(*String::Utf8Value(args[0]->ToString()), oid, &oidLength)) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - cannot parse oid")));
  }

  Local<Array> result = v8::Array::New(oidLength);
  for (size_t i = 0; i < oidLength; ++i) {
    result->Set(i,
        v8::Integer::NewFromUnsigned(
          static_cast<uint32_t>(oid[i] & 0xFFFFFFFF)));
  }
  return kScope.Close(result);
}
// }}}

// v8::Handle<v8::Value> parse_oid_wrapper(const Arguments& args) {{{
v8::Handle<v8::Value> parse_oid_wrapper(const Arguments& args) {
  HandleScope kScope;

  if (args.Length() != 1) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - missing aOid")));
  }
  if (!args[0]->IsString()) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - string expected")));
  }

  oid oid[MAX_OID_LEN];
  std::size_t oidLength = MAX_OID_LEN;


  if (!snmp_parse_oid(*String::Utf8Value(args[0]->ToString()), oid, &oidLength)) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - cannot parse oid")));
  }

  Local<Array> result = v8::Array::New(oidLength);
  for (size_t i = 0; i < oidLength; ++i) {
    result->Set(i,
        v8::Integer::NewFromUnsigned(
          static_cast<uint32_t>(oid[i] & 0xFFFFFFFF)));
  }
  return kScope.Close(result);
}
// }}}






// ===== class SnmpResult : public node::ObjectWrap {{{
class SnmpResult : public node::ObjectWrap {
  public:
    static Local<Object> New(netsnmp_variable_list* var);

    static void Initialize(Handle<Object> target);
};

// Local<Object> SnmpResult::New(netsnmp_variable_list* var) {{{
Local<Object> SnmpResult::New(netsnmp_variable_list* var) {
  assert(var);
  HandleScope kScope;

  Local<Object> o = Object::New();
  o->Set(String::New("oid"),
      SnmpValue::New(ASN_OBJECT_ID,
        var->name, var->name_length * sizeof(oid)));
  o->Set(String::New("value"),
      SnmpValue::New(var->type, var->val.string, var->val_len));
  return kScope.Close(o);
}
// }}}

// void SnmpResult::Initialize(Handle<Object> target) {{{
void SnmpResult::Initialize(Handle<Object> target) {
}
// }}}

// }}}



// ==== class SnmpSession : public node::ObjectWrap {{{

class SnmpSession : public node::ObjectWrap {
  public:
    typedef Persistent<Function> callback_type;

    struct self_data {
      SnmpSession* selfPtr_;
    };

    enum req_type {
      REQ_NEXT = SNMP_MSG_GETNEXT ,
      REQ_GET = SNMP_MSG_GET,
      REQ_BULK = SNMP_MSG_GETBULK
    };

    struct req_data {
      netsnmp_pdu* pdu_;
      req_type type_;
      callback_type callback_;
    };

    typedef std::deque<req_data> queue_type;
    typedef queue_type::iterator queue_iterator;

  private:
    self_data selfData_;
    std::string hostName_;
    std::string credentials_;
    queue_type queue_;
    void* sessionHandle_;
    SnmpSessionManager* manager_;
    Persistent<Value> destructorInvoker_;

  private: // ctors
    SnmpSession() {
      selfData_.selfPtr_ = this;
      manager_ = SnmpSessionManager::default_inst();
#ifdef ENABLE_DEBUG_PRINTS
      fprintf(stdout, "SnmpSession()\n");
#endif
    };

  private: // methods
    Handle<Value> PerformRequestImpl(
        req_type aType, netsnmp_pdu* pdu, callback_type aCallback);

    void snmp_success_cb(
        struct snmp_pdu* pdu,
        const req_data& magic
        );

    void snmp_fail_cb(
        struct snmp_pdu* pdu,
        const req_data& magic,
        const char* reason
        );

    int snmp_cb_proxy(
        int operation,
        netsnmp_session* session,
        int reqid,
        struct snmp_pdu* pdu
        );

    // void getBulk(
    //     const oid* aOID, std::size_t aOIDLen,
    //     const callback_type& aCallback);

    /**
     * We can't mix sync and async queries in same session (they would use same
     * descriptor, and we don't want responses to earlier requests, or to allow
     * node to process  other async operations, when in sync  mode). Since sync
     * version is inefficient by definition, it doesn't matter if we add little
     * more inefficiency and open another session to same host.
     */
    SnmpSession* Clone(SnmpSessionManager* aManager);

  private: // static callback proxy
    static int snmp_cb(
        int operation,
        netsnmp_session* session,
        int reqid,
        struct snmp_pdu* pdu,
        void* magic);

  private: // static interface for V8 integration
    static Persistent<v8::FunctionTemplate> constructorTemplate_;

    static Handle<Value> PerformRequest(req_type aType, const Arguments& args);

    // entrypoints from V8, all just call
    static Handle<Value> Get(const Arguments& args);
    static Handle<Value> GetNext(const Arguments& args);
    static Handle<Value> GetBulk(const Arguments& args);

    static SnmpSession* New(const std::string& hostName,
        const std::string& credentials);

    static void Destroy(Persistent<Value> v, void* param);

  public:
    ~SnmpSession() {
#ifdef ENABLE_DEBUG_PRINTS
      fprintf(stdout, "~SnmpSession()\n");
#endif
      if (sessionHandle_) {
#ifdef ENABLE_DEBUG_PRINTS
        fprintf(stdout, "close handle %p\n", sessionHandle_);
#endif
        snmp_sess_close(sessionHandle_);
        sessionHandle_ = NULL;
      }
    }

    static Handle<Value> New(const Arguments& args);
    static void Initialize(Handle<Object> target);
};

Persistent<v8::FunctionTemplate> SnmpSession::constructorTemplate_;

// SnmpSession* SnmpSession::Clone(SnmpSessionManager* aManager) {{{
SnmpSession* SnmpSession::Clone(SnmpSessionManager* aManager) {
  SnmpSession* kResult = SnmpSession::New(hostName_, credentials_);
  kResult->manager_ = aManager;
  return kResult;
}
// }}}

// Handle<Value> SnmpSession::PerformRequestImpl(...) {{{
Handle<Value> SnmpSession::PerformRequestImpl(
    req_type aType, netsnmp_pdu* pdu, callback_type aCallback)
{
  HandleScope kScope;

  // net-snmp takes over the pdu pointer!
  if (!snmp_sess_send(sessionHandle_, pdu)) {
    return kScope.Close(
        v8::ThrowException(NODE_PSYMBOL("cannot send query")));
  }
  queue_.resize(queue_.size() + 1);
  queue_.back().pdu_ = pdu;
  queue_.back().type_ = aType;
  queue_.back().callback_ = v8::Persistent<Function>::New(aCallback);
  if (queue_.size() == 1) {
    manager_->addClient(sessionHandle_);
  }
  return kScope.Close(v8::Undefined());
}
// }}}

// void SnmpSession::snmp_success_cb(...) {{{
void SnmpSession::snmp_success_cb(
    struct snmp_pdu* pdu,
    const req_data& magic
    )
{
  HandleScope kScope;

  netsnmp_variable_list* var = pdu->variables;
  Local<Array> kResult = v8::Array::New(0);
  uint32_t index = 0;

  for(; var; var = var->next_variable, ++index) {
    kResult->Set(index, SnmpResult::New(var));
  }

  Handle<Value> args[2];
  args[0] = v8::Boolean::New(false);
  args[1] = kResult;

  {
    TryCatch try_catch;

    magic.callback_->Call(v8::Context::GetCurrent()->Global(), 2, args);

    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }
}
// }}}

// void SnmpSession::snmp_fail_cb(...) {{{
void SnmpSession::snmp_fail_cb(
        struct snmp_pdu* pdu,
        const req_data& magic,
        const char* reason
        )
{
  HandleScope kScope;

  Handle<Value> args[2];
  args[0] = v8::String::NewSymbol(reason, strlen(reason));
  args[1] = v8::Null();

  {
    // TryCatch try_catch;

    magic.callback_->Call(v8::Context::GetCurrent()->Global(), 2, args);

    // if (try_catch.HasCaught()) {
    //   node::FatalException(try_catch);
    // }
  }
}
// }}}

// int SnmpSession::snmp_cb_proxy(...) {{{
int SnmpSession::snmp_cb_proxy(
    int operation,
    netsnmp_session* session,
    int reqid,
    struct snmp_pdu* pdu)
{
  queue_iterator it_end = queue_.end();
  for (queue_iterator it = queue_.begin(); it != it_end; ++it) {
    if (it->pdu_->reqid == reqid) {
      // in  some more  extreme  situations, *this  can  be deallocated  inside
      // callback (by forcing GC cycle). Everything we want to do with instance
      // must be done before trying callback.
      req_data kReq = *it;

      queue_.erase(it);
      if (queue_.size() == 0) {
        manager_->removeClient(sessionHandle_);
      }

      if (operation != NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
        const char* msg;
        switch (operation) {
          case NETSNMP_CALLBACK_OP_TIMED_OUT:
            msg = "timeout";
            break;
          case NETSNMP_CALLBACK_OP_SEND_FAILED:
            msg = "send failed";
            break;
          case NETSNMP_CALLBACK_OP_CONNECT:
            msg = "connect failed";
            break;
          case NETSNMP_CALLBACK_OP_DISCONNECT:
            msg = "peer has disconnected";
            break;
          default:
            msg = "unknown snmp error";
        }
        snmp_fail_cb(pdu, kReq, msg);
        kReq.callback_.Dispose();
        return 1;
      }
      if (pdu->errstat != SNMP_ERR_NOERROR) {
        const char* msg = snmp_errstring(pdu->errstat);
        snmp_fail_cb(pdu, kReq, msg);
        kReq.callback_.Dispose();
        return 1;
      }

      switch (it->type_) {
        case REQ_GET:
          snmp_success_cb(pdu, kReq);
          break;
        case REQ_NEXT:
          snmp_success_cb(pdu, kReq);
          break;
        case REQ_BULK:
        default:
          assert(false && "internal error: inconsistent req_data record");
          snmp_fail_cb(pdu, kReq,
              "internal error: inconsistent req_data record");
          break;
      }
      kReq.callback_.Dispose();
      return 1;
    }
  }
  assert(false && "spurious response received");
  return 1;
}
// }}}

// void SnmpSession::snmp_cb(...) {{{
int SnmpSession::snmp_cb(
    int operation,
    netsnmp_session* session,
    int reqid,
    struct snmp_pdu* pdu,
    void* magic)
{
  SnmpSession::self_data* instData =
    reinterpret_cast<SnmpSession::self_data*>(magic);
  return instData->selfPtr_->snmp_cb_proxy(operation, session, reqid, pdu);
}
// }}}


// SnmpSession* SnmpSession::New(hostname, community) {{{
SnmpSession* SnmpSession::New(const std::string& hostName,
    const std::string& credentials)
{
  SnmpSession* kResult = new SnmpSession();
  kResult->hostName_ = hostName;
  kResult->credentials_ = credentials;

  netsnmp_session kSession;
  snmp_sess_init(&kSession);
  kSession.peername = strdup(kResult->hostName_.c_str());
  kSession.version = SNMP_VERSION_1;

  kSession.community = (u_char*)malloc(kResult->credentials_.size() + 1);
  // TODO: some better way to report out of memory instead of SIGSEGV?
  memcpy(kSession.community, kResult->credentials_.c_str(),
      kResult->credentials_.size() + 1);
  kSession.community_len = kResult->credentials_.size();

  kSession.callback = SnmpSession::snmp_cb;
  kSession.callback_magic = &kResult->selfData_;

  kResult->sessionHandle_ = snmp_sess_open(&kSession);
#ifdef ENABLE_DEBUG_PRINTS
  fprintf(stderr, "new session handle %p\n", kResult->sessionHandle_);
#endif
  kResult->manager_ = SnmpSessionManager::default_inst();
  free(kSession.community);
  free(kSession.peername);

  if (!kResult->sessionHandle_) {
    delete kResult;
    return NULL;
  }
  return kResult;
}
// }}}

// Handle<Value> SnmpSession::New(const Arguments& args) {{{
Handle<Value> SnmpSession::New(const Arguments& args) {
  HandleScope kScope;
  std::auto_ptr<SnmpSession> kInst;

  if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString()) {
    return kScope.Close(v8::ThrowException(
        NODE_PSYMBOL("not enough arguments or wrong type"
        " (expecting two strings)")));
  }

  {
    v8::String::Utf8Value hostname(args[0]->ToString());
    v8::String::Utf8Value credentials(args[1]->ToString());
    kInst.reset(SnmpSession::New(
        std::string(*hostname, hostname.length()),
        std::string(*credentials, credentials.length())
        ));
  }

  if (!kInst.get()) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("cannot open snmp session")));
  }

  kInst->Wrap(args.This());
  /* Persistent<Object> p = */
  kInst->destructorInvoker_ = Persistent<Object>::New(args.This());
  kInst->destructorInvoker_.MakeWeak(NULL, SnmpSession::Destroy);
  kInst.release();
  return kScope.Close(args.This());
}
// }}}

// void SnmpSession::Destroy(Persistent<Value> v, void* param) {{{
void SnmpSession::Destroy(Persistent<Value> v, void* param) {
  assert(v->IsObject());
  SnmpSession* s = ObjectWrap::Unwrap<SnmpSession>(v->ToObject());
#ifdef ENABLE_DEBUG_PRINTS
  fprintf(stderr, "destroy persistent for %p\n", s);
#endif
  delete s;
  v.Dispose();
}
// }}}

namespace {
// Local<Value> addNullVarFromV8Array(...) {{{
void addNullVarFromV8Array(netsnmp_pdu* pdu, Local<Value> var,
    std::vector<oid>* tmp)
{
  // handleScope - intentionally omited, use scope from PerformRequest
  if (!var->IsArray()) {
    v8::ThrowException(
        NODE_PSYMBOL("invalid argument - not an array"));
    return;
  }
  Local<Array> a = Local<Array>::Cast(var);
  size_t end = a->Length();
  if (end == 0) {
    v8::ThrowException(
        NODE_PSYMBOL("invalid argument - empty oid"));
    return;
  }
  tmp->resize(end);
  for (size_t i = 0; i < end; ++i) {
    Local<Value> v = a->Get(i);
    if (!v->IsUint32()) {
      v8::ThrowException(
          NODE_PSYMBOL("invalid oid - non-integer member"));
      return;
    }
    (*tmp)[i] = v->ToUint32()->Value();
  }
  if (!snmp_add_null_var(pdu, &((*tmp)[0]), tmp->size())) {
    v8::ThrowException(NODE_PSYMBOL("cannot add query to pdu"));
    return;
  }
}
// }}}
}

// Handle<Value> SnmpSession::PerformRequest(...) {{{
Handle<Value> SnmpSession::PerformRequest(
    req_type aType, const Arguments& args)
{
  HandleScope kScope;
  SnmpSession* inst = ObjectWrap::Unwrap<SnmpSession>(args.This());

  // call with (OID, callback, bool (=sync or not sync))
  if (args.Length() < 3) {
    return kScope.Close(v8::ThrowException(NODE_PSYMBOL("missing arguments")));
  }
  if (!args[0]->IsArray()) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - only string OID is supported")));
  }
  if (!args[1]->IsFunction()) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid arguments - callback is not a function")));
  }
  if (!args[2]->IsBoolean()) {
    return kScope.Close(v8::ThrowException(
          NODE_PSYMBOL("invalid argument - sync flag must be boolean")));
  }

  Local<Array> kOidArg = Local<Array>::Cast(args[0]);

  netsnmp_pdu* pdu = NULL;

  {
    size_t end = kOidArg->Length();
    if (end == 0) {
      return kScope.Close(v8::ThrowException(
            NODE_PSYMBOL("invalid argument - empty oid")));
    }

    pdu = snmp_pdu_create(static_cast<int>(aType));
    if (!pdu) {
      return kScope.Close(
          v8::ThrowException(NODE_PSYMBOL("cannot allocate pdu")));
    }

    std::vector<oid> tmp;
    v8::TryCatch tryCatch;

    if (kOidArg->Get(0)->IsArray()) {
      // array of arrays - second level arrays must contain integers
      for (size_t i = 0; i < end; ++i) {
        addNullVarFromV8Array(pdu, kOidArg->Get(i), &tmp);
        if (tryCatch.HasCaught()) {
          return kScope.Close(tryCatch.ReThrow());
        }
      }
    } else {
      // array of integers - single oid query
      addNullVarFromV8Array(pdu, kOidArg, &tmp);
      if (tryCatch.HasCaught()) {
        return kScope.Close(tryCatch.ReThrow());
      }
    }
  }

  if (args[2]->BooleanValue()) {
#if EV_MULTIPLICITY
    struct ev_loop* our_loop = ev_loop_new(0);
    SnmpSessionManager* manager = SnmpSessionManager::create(our_loop);
    SnmpSession* cloned_sess = inst->Clone(manager);

    cloned_sess->PerformRequestImpl(aType, pdu,
        Persistent<Function>(Function::Cast(*args[1])));

#if EV_VERSION_MAJOR == 3
    ev_loop(our_loop, 0);
#else
    ev_run(our_loop);
#endif

    delete cloned_sess;
    delete manager;
    ev_loop_destroy(our_loop);
#else
    return kScope.Close(
        v8::ThrowException(
          NODE_PSYMBOL("synchronous interface is unavailable due to"
            " binding module configuration")));
#endif
  } else {
    inst->PerformRequestImpl(aType, pdu,
        Persistent<Function>(Function::Cast(*args[1])));
  }
  return kScope.Close(v8::Undefined());
}
// }}}

// Handle<Value> SnmpSession::Get(const Arguments& args) {{{
Handle<Value> SnmpSession::Get(const Arguments& args) {
  return PerformRequest(REQ_GET, args);
}
// }}}

// Handle<Value> SnmpSession::GetNext(const Arguments& args) {{{
Handle<Value> SnmpSession::GetNext(const Arguments& args) {
  return PerformRequest(REQ_NEXT, args);
}
// }}}

// Handle<Value> SnmpSession::GetBulk(const Arguments& args) {{{
Handle<Value> SnmpSession::GetBulk(const Arguments& args) {
  return PerformRequest(REQ_BULK, args);
}
// }}}


// void SnmpSession::Initialize(Handle<Object> target) {{{
void SnmpSession::Initialize(Handle<Object> target) {
  js::HandleScope kScope;

  Local<FunctionTemplate> t = FunctionTemplate::New(SnmpSession::New);
  constructorTemplate_ = Persistent<FunctionTemplate>::New(t);
  constructorTemplate_->InstanceTemplate()->SetInternalFieldCount(1);
  constructorTemplate_->SetClassName(String::NewSymbol("Connection"));

  // t->Inherit(EventEmitter::constructor_template);

  NODE_SET_PROTOTYPE_METHOD(t, "Get", SnmpSession::Get);
  NODE_SET_PROTOTYPE_METHOD(t, "GetNext", SnmpSession::GetNext);

  target->Set(String::NewSymbol("Connection"),
      constructorTemplate_->GetFunction());
}
// }}}

// }}}


extern "C" void
init (Handle<Object> target) {
  HandleScope scope;

  init_snmp("asdf");

  SnmpSession::Initialize(target);
  SnmpValue::Initialize(target);
  SnmpResult::Initialize(target);

  NODE_SET_METHOD(target, "read_objid", read_objid_wrapper);
  NODE_SET_METHOD(target, "parse_oid", parse_oid_wrapper);
}

// vim: ts=2 fdm=marker syntax=cpp expandtab sw=2

