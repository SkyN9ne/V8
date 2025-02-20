// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_OBJECTS_VISITING_INL_H_
#define V8_HEAP_OBJECTS_VISITING_INL_H_

#include "src/base/logging.h"
#include "src/heap/mark-compact.h"
#include "src/heap/objects-visiting.h"
#include "src/objects/arguments.h"
#include "src/objects/data-handler-inl.h"
#include "src/objects/free-space-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/objects/module-inl.h"
#include "src/objects/objects-body-descriptors-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/oddball.h"
#include "src/objects/ordered-hash-table.h"
#include "src/objects/synthetic-module-inl.h"
#include "src/objects/torque-defined-classes.h"
#include "src/objects/visitors.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-objects.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

template <typename ResultType, typename ConcreteVisitor>
HeapVisitor<ResultType, ConcreteVisitor>::HeapVisitor(
    PtrComprCageBase cage_base, PtrComprCageBase code_cage_base)
    : ObjectVisitorWithCageBases(cage_base, code_cage_base) {}

template <typename ResultType, typename ConcreteVisitor>
HeapVisitor<ResultType, ConcreteVisitor>::HeapVisitor(Isolate* isolate)
    : ObjectVisitorWithCageBases(isolate) {}

template <typename ResultType, typename ConcreteVisitor>
HeapVisitor<ResultType, ConcreteVisitor>::HeapVisitor(Heap* heap)
    : ObjectVisitorWithCageBases(heap) {}

template <typename ResultType, typename ConcreteVisitor>
template <typename T>
T HeapVisitor<ResultType, ConcreteVisitor>::Cast(HeapObject object) {
  return T::cast(object);
}

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::Visit(HeapObject object) {
  return Visit(object.map(cage_base()), object);
}

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::Visit(Map map,
                                                           HeapObject object) {
  ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);
  switch (map.visitor_id()) {
#define CASE(TypeName)               \
  case kVisit##TypeName:             \
    return visitor->Visit##TypeName( \
        map, ConcreteVisitor::template Cast<TypeName>(object));
    TYPED_VISITOR_ID_LIST(CASE)
    TORQUE_VISITOR_ID_LIST(CASE)
#undef CASE
    case kVisitShortcutCandidate:
      return visitor->VisitShortcutCandidate(
          map, ConcreteVisitor::template Cast<ConsString>(object));
    case kVisitDataObject:
      return visitor->VisitDataObject(map, object);
    case kVisitJSObjectFast:
      return visitor->VisitJSObjectFast(
          map, ConcreteVisitor::template Cast<JSObject>(object));
    case kVisitJSApiObject:
      return visitor->VisitJSApiObject(
          map, ConcreteVisitor::template Cast<JSObject>(object));
    case kVisitStruct:
      return visitor->VisitStruct(map, object);
    case kVisitFreeSpace:
      return visitor->VisitFreeSpace(map, FreeSpace::cast(object));
    case kDataOnlyVisitorIdCount:
    case kVisitorIdCount:
      UNREACHABLE();
  }
  UNREACHABLE();
  // Make the compiler happy.
  return ResultType();
}

template <typename ResultType, typename ConcreteVisitor>
void HeapVisitor<ResultType, ConcreteVisitor>::VisitMapPointer(
    HeapObject host) {
  DCHECK(!host.map_word(kRelaxedLoad).IsForwardingAddress());
  if (!static_cast<ConcreteVisitor*>(this)->ShouldVisitMapPointer()) return;
  static_cast<ConcreteVisitor*>(this)->VisitMapPointer(host);
}

#define VISIT(TypeName)                                                        \
  template <typename ResultType, typename ConcreteVisitor>                     \
  ResultType HeapVisitor<ResultType, ConcreteVisitor>::Visit##TypeName(        \
      Map map, TypeName object) {                                              \
    ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);            \
    if (!visitor->ShouldVisit(object)) return ResultType();                    \
    if (!visitor->AllowDefaultJSObjectVisit()) {                               \
      DCHECK_WITH_MSG(!map.IsJSObjectMap(),                                    \
                      "Implement custom visitor for new JSObject subclass in " \
                      "concurrent marker");                                    \
    }                                                                          \
    int size = TypeName::BodyDescriptor::SizeOf(map, object);                  \
    if (visitor->ShouldVisitMapPointer()) {                                    \
      visitor->VisitMapPointer(object);                                        \
    }                                                                          \
    TypeName::BodyDescriptor::IterateBody(map, object, size, visitor);         \
    return static_cast<ResultType>(size);                                      \
  }
TYPED_VISITOR_ID_LIST(VISIT)
TORQUE_VISITOR_ID_LIST(VISIT)
#undef VISIT

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::VisitShortcutCandidate(
    Map map, ConsString object) {
  return static_cast<ConcreteVisitor*>(this)->VisitConsString(map, object);
}

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::VisitDataObject(
    Map map, HeapObject object) {
  ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);
  if (!visitor->ShouldVisit(object)) return ResultType();
  int size = map.instance_size();
  if (visitor->ShouldVisitMapPointer()) {
    visitor->VisitMapPointer(object);
  }
#ifdef V8_ENABLE_SANDBOX
  // The following types have external pointers, which must be visited.
  // TODO(v8:10391) Consider adding custom visitor IDs for these and making this
  // block not depend on V8_ENABLE_SANDBOX.
  if (object.IsForeign(cage_base())) {
    Foreign::BodyDescriptor::IterateBody(map, object, size, visitor);
  }
#endif  // V8_ENABLE_SANDBOX
  return static_cast<ResultType>(size);
}

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::VisitJSObjectFast(
    Map map, JSObject object) {
  ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);
  if (!visitor->ShouldVisit(object)) return ResultType();
  int size = JSObject::FastBodyDescriptor::SizeOf(map, object);
  if (visitor->ShouldVisitMapPointer()) {
    visitor->VisitMapPointer(object);
  }
  JSObject::FastBodyDescriptor::IterateBody(map, object, size, visitor);
  return static_cast<ResultType>(size);
}

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::VisitJSApiObject(
    Map map, JSObject object) {
  ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);
  if (!visitor->ShouldVisit(object)) return ResultType();
  int size = JSObject::BodyDescriptor::SizeOf(map, object);
  if (visitor->ShouldVisitMapPointer()) {
    visitor->VisitMapPointer(object);
  }
  JSObject::BodyDescriptor::IterateBody(map, object, size, visitor);
  return static_cast<ResultType>(size);
}

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::VisitStruct(
    Map map, HeapObject object) {
  ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);
  if (!visitor->ShouldVisit(object)) return ResultType();
  int size = map.instance_size();
  if (visitor->ShouldVisitMapPointer()) {
    visitor->VisitMapPointer(object);
  }
  StructBodyDescriptor::IterateBody(map, object, size, visitor);
  return static_cast<ResultType>(size);
}

template <typename ResultType, typename ConcreteVisitor>
ResultType HeapVisitor<ResultType, ConcreteVisitor>::VisitFreeSpace(
    Map map, FreeSpace object) {
  ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);
  if (!visitor->ShouldVisit(object)) return ResultType();
  if (visitor->ShouldVisitMapPointer()) {
    visitor->VisitMapPointer(object);
  }
  return static_cast<ResultType>(object.size(kRelaxedLoad));
}

template <typename ConcreteVisitor>
NewSpaceVisitor<ConcreteVisitor>::NewSpaceVisitor(Isolate* isolate)
    : HeapVisitor<int, ConcreteVisitor>(isolate) {}

template <typename ConcreteVisitor>
int NewSpaceVisitor<ConcreteVisitor>::VisitNativeContext(Map map,
                                                         NativeContext object) {
  // There should be no native contexts in new space.
  UNREACHABLE();
}

template <typename ConcreteVisitor>
int NewSpaceVisitor<ConcreteVisitor>::VisitJSApiObject(Map map,
                                                       JSObject object) {
  ConcreteVisitor* visitor = static_cast<ConcreteVisitor*>(this);
  return visitor->VisitJSApiObject(map, object);
}

template <typename ConcreteVisitor>
int NewSpaceVisitor<ConcreteVisitor>::VisitSharedFunctionInfo(
    Map map, SharedFunctionInfo object) {
  UNREACHABLE();
  return 0;
}

template <typename ConcreteVisitor>
int NewSpaceVisitor<ConcreteVisitor>::VisitWeakCell(Map map,
                                                    WeakCell weak_cell) {
  UNREACHABLE();
  return 0;
}

template <typename ConcreteVisitor>
template <typename T, typename TBodyDescriptor>
int NewSpaceVisitor<ConcreteVisitor>::VisitJSObjectSubclass(Map map, T object) {
  if (!static_cast<ConcreteVisitor*>(this)->ShouldVisit(object)) return 0;
  this->VisitMapPointer(object);
  int size = TBodyDescriptor::SizeOf(map, object);
  TBodyDescriptor::IterateBody(map, object, size, this);
  return size;
}

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_OBJECTS_VISITING_INL_H_
