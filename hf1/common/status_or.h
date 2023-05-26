#ifndef STATUS_OR_INCLUDED_
#define STATUS_OR_INCLUDED_

enum Status {
  kSuccess, kUnavailableError
};

template<typename ValueType> class StatusOr {
public:
  StatusOr(ValueType &&v) : status_(kSuccess), value_(v) {}
  StatusOr(Status e) : status_(e) {}
  
  ValueType *operator->() { return &value_; }
  ValueType &operator*() { return value_; }

  bool ok() const { return status_ == kSuccess; }

private:
  Status status_;
  ValueType value_;
};

#endif  // STATUS_OR_INCLUDED_