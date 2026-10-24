// SPDX-License-Identifier: Apache-2.0 OR MIT
// TEST AID ONLY. A minimal declaration of the part of React Native's jsi/jsi.h that LataiJsi.cpp
// uses, with the real signatures, so that `clang++ -fsyntax-only` can check the glue without a React
// Native checkout. It proves the glue is consistent with these declarations, not with the real
// header. Never linked, never shipped.
#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>

namespace facebook::jsi {

class Runtime;
class Value;
class Function;
class String;
class ArrayBuffer;

class PropNameID {
 public:
  static PropNameID forAscii(Runtime &, const char *str);
};

class String {
 public:
  static String createFromUtf8(Runtime &, const std::string &utf8);
  std::string utf8(Runtime &) const;
};

class Object {
 public:
  explicit Object(Runtime &);
  Function getPropertyAsFunction(Runtime &, const char *name) const;
  template <typename T>
  void setProperty(Runtime &, const char *name, T &&value);
  bool isArrayBuffer(Runtime &) const;
  ArrayBuffer getArrayBuffer(Runtime &) const &;
  Function asFunction(Runtime &) const &;
  Function asFunction(Runtime &) &&;
};

class ArrayBuffer : public Object {
 public:
  uint8_t *data(Runtime &);
  size_t size(Runtime &);
};

class Value {
 public:
  Value(double);
  Value(bool);
  Value(Value &&);
  Value(Runtime &, const String &);
  Value(Object &&);
  Value(String &&);
  Value(Function &&);
  static Value undefined();
  static Value null();
  bool isString() const;
  bool isNumber() const;
  bool isBool() const;
  bool isObject() const;
  double getNumber() const;
  bool getBool() const;
  String asString(Runtime &) const &;
  Object asObject(Runtime &) const &;
};

using HostFunctionType = std::function<Value(Runtime &rt, const Value &thisVal, const Value *args, size_t count)>;

class Function : public Object {
 public:
  Function(Function &&);
  static Function createFromHostFunction(Runtime &, const PropNameID &name, unsigned int paramCount, HostFunctionType func);
  template <typename... Args>
  Value call(Runtime &, Args &&...args) const;
  template <typename... Args>
  Value callAsConstructor(Runtime &, Args &&...args) const;
};

class Runtime {
 public:
  Object global();
};

class JSError : public std::exception {
 public:
  JSError(Runtime &, std::string message);
};

template <typename T>
void Object::setProperty(Runtime &, const char *, T &&) {}
template <typename... Args>
Value Function::call(Runtime &, Args &&...) const { return Value::undefined(); }
template <typename... Args>
Value Function::callAsConstructor(Runtime &, Args &&...) const { return Value::undefined(); }

}  // namespace facebook::jsi
