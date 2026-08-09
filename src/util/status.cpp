#include "laminar/status.h"

#include <string>

namespace laminar {

std::string Status::ToString() const {
  std::string label;
  switch (code_) {
  case Code::kOk:
    return "OK";
  case Code::kNotFound:
    label = "NotFound";
    break;
  case Code::kCorruption:
    label = "Corruption";
    break;
  case Code::kIOError:
    label = "IOError";
    break;
  case Code::kInvalidArgument:
    label = "InvalidArgument";
    break;
  case Code::kNotSupported:
    label = "NotSupported";
    break;
  case Code::kAlreadyExists:
    label = "AlreadyExists";
    break;
  case Code::kFull:
    label = "Full";
    break;
  }
  return message_.empty() ? label : label + ": " + message_;
}

} // namespace laminar
