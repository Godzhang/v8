// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"

#include "src/ic/call-optimization.h"
#include "src/ic/ic-inl.h"
#include "src/ic/ic-compiler.h"


namespace v8 {
namespace internal {


Handle<Code> PropertyICCompiler::Find(Handle<Name> name,
                                      Handle<Map> stub_holder, Code::Kind kind,
                                      ExtraICState extra_state,
                                      CacheHolderFlag cache_holder) {
  Code::Flags flags =
      Code::ComputeMonomorphicFlags(kind, extra_state, cache_holder);
  Object* probe = stub_holder->FindInCodeCache(*name, flags);
  if (probe->IsCode()) return handle(Code::cast(probe));
  return Handle<Code>::null();
}


Handle<Code> PropertyHandlerCompiler::Find(Handle<Name> name,
                                           Handle<Map> stub_holder,
                                           Code::Kind kind,
                                           CacheHolderFlag cache_holder,
                                           Code::StubType type) {
  Code::Flags flags = Code::ComputeHandlerFlags(kind, type, cache_holder);
  Object* probe = stub_holder->FindInCodeCache(*name, flags);
  if (probe->IsCode()) return handle(Code::cast(probe));
  return Handle<Code>::null();
}


Handle<Code> PropertyICCompiler::ComputeMonomorphic(
    Code::Kind kind, Handle<Name> name, Handle<HeapType> type,
    Handle<Code> handler, ExtraICState extra_ic_state) {
  Isolate* isolate = name->GetIsolate();
  if (handler.is_identical_to(isolate->builtins()->LoadIC_Normal()) ||
      handler.is_identical_to(isolate->builtins()->StoreIC_Normal())) {
    name = isolate->factory()->normal_ic_symbol();
  }

  CacheHolderFlag flag;
  Handle<Map> stub_holder = IC::GetICCacheHolder(*type, isolate, &flag);

  Handle<Code> ic;
  // There are multiple string maps that all use the same prototype. That
  // prototype cannot hold multiple handlers, one for each of the string maps,
  // for a single name. Hence, turn off caching of the IC.
  bool can_be_cached = !type->Is(HeapType::String());
  if (can_be_cached) {
    ic = Find(name, stub_holder, kind, extra_ic_state, flag);
    if (!ic.is_null()) return ic;
  }

#ifdef DEBUG
  if (kind == Code::KEYED_STORE_IC) {
    DCHECK(STANDARD_STORE ==
           KeyedStoreIC::GetKeyedAccessStoreMode(extra_ic_state));
  }
#endif

  PropertyICCompiler ic_compiler(isolate, kind, extra_ic_state, flag);
  ic = ic_compiler.CompileMonomorphic(type, handler, name, PROPERTY);

  if (can_be_cached) Map::UpdateCodeCache(stub_holder, name, ic);
  return ic;
}


Handle<Code> NamedLoadHandlerCompiler::ComputeLoadNonexistent(
    Handle<Name> name, Handle<HeapType> type) {
  Isolate* isolate = name->GetIsolate();
  Handle<Map> receiver_map = IC::TypeToMap(*type, isolate);
  if (receiver_map->prototype()->IsNull()) {
    // TODO(jkummerow/verwaest): If there is no prototype and the property
    // is nonexistent, introduce a builtin to handle this (fast properties
    // -> return undefined, dictionary properties -> do negative lookup).
    return Handle<Code>();
  }
  CacheHolderFlag flag;
  Handle<Map> stub_holder_map =
      IC::GetHandlerCacheHolder(*type, false, isolate, &flag);

  // If no dictionary mode objects are present in the prototype chain, the load
  // nonexistent IC stub can be shared for all names for a given map and we use
  // the empty string for the map cache in that case. If there are dictionary
  // mode objects involved, we need to do negative lookups in the stub and
  // therefore the stub will be specific to the name.
  Handle<Name> cache_name =
      receiver_map->is_dictionary_map()
          ? name
          : Handle<Name>::cast(isolate->factory()->nonexistent_symbol());
  Handle<Map> current_map = stub_holder_map;
  Handle<JSObject> last(JSObject::cast(receiver_map->prototype()));
  while (true) {
    if (current_map->is_dictionary_map()) cache_name = name;
    if (current_map->prototype()->IsNull()) break;
    last = handle(JSObject::cast(current_map->prototype()));
    current_map = handle(last->map());
  }
  // Compile the stub that is either shared for all names or
  // name specific if there are global objects involved.
  Handle<Code> handler = PropertyHandlerCompiler::Find(
      cache_name, stub_holder_map, Code::LOAD_IC, flag, Code::FAST);
  if (!handler.is_null()) return handler;

  NamedLoadHandlerCompiler compiler(isolate, type, last, flag);
  handler = compiler.CompileLoadNonexistent(cache_name);
  Map::UpdateCodeCache(stub_holder_map, cache_name, handler);
  return handler;
}


Handle<Code> PropertyICCompiler::ComputeKeyedLoadMonomorphic(
    Handle<Map> receiver_map) {
  Isolate* isolate = receiver_map->GetIsolate();
  Code::Flags flags = Code::ComputeMonomorphicFlags(Code::KEYED_LOAD_IC);
  Handle<Name> name = isolate->factory()->KeyedLoadMonomorphic_string();

  Handle<Object> probe(receiver_map->FindInCodeCache(*name, flags), isolate);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  ElementsKind elements_kind = receiver_map->elements_kind();
  Handle<Code> stub;
  if (receiver_map->has_fast_elements() ||
      receiver_map->has_external_array_elements() ||
      receiver_map->has_fixed_typed_array_elements()) {
    stub = LoadFastElementStub(isolate,
                               receiver_map->instance_type() == JS_ARRAY_TYPE,
                               elements_kind).GetCode();
  } else {
    stub = FLAG_compiled_keyed_dictionary_loads
               ? LoadDictionaryElementStub(isolate).GetCode()
               : LoadDictionaryElementPlatformStub(isolate).GetCode();
  }
  PropertyICCompiler compiler(isolate, Code::KEYED_LOAD_IC);
  Handle<Code> code =
      compiler.CompileMonomorphic(HeapType::Class(receiver_map, isolate), stub,
                                  isolate->factory()->empty_string(), ELEMENT);

  Map::UpdateCodeCache(receiver_map, name, code);
  return code;
}


Handle<Code> PropertyICCompiler::ComputeKeyedStoreMonomorphic(
    Handle<Map> receiver_map, StrictMode strict_mode,
    KeyedAccessStoreMode store_mode) {
  Isolate* isolate = receiver_map->GetIsolate();
  ExtraICState extra_state =
      KeyedStoreIC::ComputeExtraICState(strict_mode, store_mode);
  Code::Flags flags =
      Code::ComputeMonomorphicFlags(Code::KEYED_STORE_IC, extra_state);

  DCHECK(store_mode == STANDARD_STORE ||
         store_mode == STORE_AND_GROW_NO_TRANSITION ||
         store_mode == STORE_NO_TRANSITION_IGNORE_OUT_OF_BOUNDS ||
         store_mode == STORE_NO_TRANSITION_HANDLE_COW);

  Handle<String> name = isolate->factory()->KeyedStoreMonomorphic_string();
  Handle<Object> probe(receiver_map->FindInCodeCache(*name, flags), isolate);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  PropertyICCompiler compiler(isolate, Code::KEYED_STORE_IC, extra_state);
  Handle<Code> code =
      compiler.CompileKeyedStoreMonomorphic(receiver_map, store_mode);

  Map::UpdateCodeCache(receiver_map, name, code);
  DCHECK(KeyedStoreIC::GetKeyedAccessStoreMode(code->extra_ic_state()) ==
         store_mode);
  return code;
}


Code* PropertyICCompiler::FindPreMonomorphic(Isolate* isolate, Code::Kind kind,
                                             ExtraICState state) {
  Code::Flags flags = Code::ComputeFlags(kind, PREMONOMORPHIC, state);
  UnseededNumberDictionary* dictionary =
      isolate->heap()->non_monomorphic_cache();
  int entry = dictionary->FindEntry(isolate, flags);
  DCHECK(entry != -1);
  Object* code = dictionary->ValueAt(entry);
  // This might be called during the marking phase of the collector
  // hence the unchecked cast.
  return reinterpret_cast<Code*>(code);
}


static void FillCache(Isolate* isolate, Handle<Code> code) {
  Handle<UnseededNumberDictionary> dictionary = UnseededNumberDictionary::Set(
      isolate->factory()->non_monomorphic_cache(), code->flags(), code);
  isolate->heap()->public_set_non_monomorphic_cache(*dictionary);
}


Handle<Code> PropertyICCompiler::ComputeLoad(Isolate* isolate,
                                             InlineCacheState ic_state,
                                             ExtraICState extra_state) {
  Code::Flags flags = Code::ComputeFlags(Code::LOAD_IC, ic_state, extra_state);
  Handle<UnseededNumberDictionary> cache =
      isolate->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  PropertyICCompiler compiler(isolate, Code::LOAD_IC);
  Handle<Code> code;
  if (ic_state == UNINITIALIZED) {
    code = compiler.CompileLoadInitialize(flags);
  } else if (ic_state == PREMONOMORPHIC) {
    code = compiler.CompileLoadPreMonomorphic(flags);
  } else if (ic_state == MEGAMORPHIC) {
    code = compiler.CompileLoadMegamorphic(flags);
  } else {
    UNREACHABLE();
  }
  FillCache(isolate, code);
  return code;
}


Handle<Code> PropertyICCompiler::ComputeStore(Isolate* isolate,
                                              InlineCacheState ic_state,
                                              ExtraICState extra_state) {
  Code::Flags flags = Code::ComputeFlags(Code::STORE_IC, ic_state, extra_state);
  Handle<UnseededNumberDictionary> cache =
      isolate->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  PropertyICCompiler compiler(isolate, Code::STORE_IC);
  Handle<Code> code;
  if (ic_state == UNINITIALIZED) {
    code = compiler.CompileStoreInitialize(flags);
  } else if (ic_state == PREMONOMORPHIC) {
    code = compiler.CompileStorePreMonomorphic(flags);
  } else if (ic_state == GENERIC) {
    code = compiler.CompileStoreGeneric(flags);
  } else if (ic_state == MEGAMORPHIC) {
    code = compiler.CompileStoreMegamorphic(flags);
  } else {
    UNREACHABLE();
  }

  FillCache(isolate, code);
  return code;
}


Handle<Code> PropertyICCompiler::ComputeCompareNil(Handle<Map> receiver_map,
                                                   CompareNilICStub* stub) {
  Isolate* isolate = receiver_map->GetIsolate();
  Handle<String> name(isolate->heap()->empty_string());
  if (!receiver_map->is_dictionary_map()) {
    Handle<Code> cached_ic =
        Find(name, receiver_map, Code::COMPARE_NIL_IC, stub->GetExtraICState());
    if (!cached_ic.is_null()) return cached_ic;
  }

  Code::FindAndReplacePattern pattern;
  pattern.Add(isolate->factory()->meta_map(), receiver_map);
  Handle<Code> ic = stub->GetCodeCopy(pattern);

  if (!receiver_map->is_dictionary_map()) {
    Map::UpdateCodeCache(receiver_map, name, ic);
  }

  return ic;
}


// TODO(verwaest): Change this method so it takes in a TypeHandleList.
Handle<Code> PropertyICCompiler::ComputeKeyedLoadPolymorphic(
    MapHandleList* receiver_maps) {
  Isolate* isolate = receiver_maps->at(0)->GetIsolate();
  Code::Flags flags = Code::ComputeFlags(Code::KEYED_LOAD_IC, POLYMORPHIC);
  Handle<PolymorphicCodeCache> cache =
      isolate->factory()->polymorphic_code_cache();
  Handle<Object> probe = cache->Lookup(receiver_maps, flags);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  TypeHandleList types(receiver_maps->length());
  for (int i = 0; i < receiver_maps->length(); i++) {
    types.Add(HeapType::Class(receiver_maps->at(i), isolate));
  }
  CodeHandleList handlers(receiver_maps->length());
  ElementHandlerCompiler compiler(isolate);
  compiler.CompileElementHandlers(receiver_maps, &handlers);
  PropertyICCompiler ic_compiler(isolate, Code::KEYED_LOAD_IC);
  Handle<Code> code = ic_compiler.CompilePolymorphic(
      &types, &handlers, isolate->factory()->empty_string(), Code::NORMAL,
      ELEMENT);

  isolate->counters()->keyed_load_polymorphic_stubs()->Increment();

  PolymorphicCodeCache::Update(cache, receiver_maps, flags, code);
  return code;
}


Handle<Code> PropertyICCompiler::ComputePolymorphic(
    Code::Kind kind, TypeHandleList* types, CodeHandleList* handlers,
    int valid_types, Handle<Name> name, ExtraICState extra_ic_state) {
  Handle<Code> handler = handlers->at(0);
  Code::StubType type = valid_types == 1 ? handler->type() : Code::NORMAL;
  DCHECK(kind == Code::LOAD_IC || kind == Code::STORE_IC);
  PropertyICCompiler ic_compiler(name->GetIsolate(), kind, extra_ic_state);
  return ic_compiler.CompilePolymorphic(types, handlers, name, type, PROPERTY);
}


Handle<Code> PropertyICCompiler::ComputeKeyedStorePolymorphic(
    MapHandleList* receiver_maps, KeyedAccessStoreMode store_mode,
    StrictMode strict_mode) {
  Isolate* isolate = receiver_maps->at(0)->GetIsolate();
  DCHECK(store_mode == STANDARD_STORE ||
         store_mode == STORE_AND_GROW_NO_TRANSITION ||
         store_mode == STORE_NO_TRANSITION_IGNORE_OUT_OF_BOUNDS ||
         store_mode == STORE_NO_TRANSITION_HANDLE_COW);
  Handle<PolymorphicCodeCache> cache =
      isolate->factory()->polymorphic_code_cache();
  ExtraICState extra_state =
      KeyedStoreIC::ComputeExtraICState(strict_mode, store_mode);
  Code::Flags flags =
      Code::ComputeFlags(Code::KEYED_STORE_IC, POLYMORPHIC, extra_state);
  Handle<Object> probe = cache->Lookup(receiver_maps, flags);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  PropertyICCompiler compiler(isolate, Code::KEYED_STORE_IC, extra_state);
  Handle<Code> code =
      compiler.CompileKeyedStorePolymorphic(receiver_maps, store_mode);
  PolymorphicCodeCache::Update(cache, receiver_maps, flags, code);
  return code;
}


Handle<Code> PropertyICCompiler::CompileLoadInitialize(Code::Flags flags) {
  LoadIC::GenerateInitialize(masm());
  Handle<Code> code = GetCodeWithFlags(flags, "CompileLoadInitialize");
  PROFILE(isolate(), CodeCreateEvent(Logger::LOAD_INITIALIZE_TAG, *code, 0));
  return code;
}


Handle<Code> PropertyICCompiler::CompileLoadPreMonomorphic(Code::Flags flags) {
  LoadIC::GeneratePreMonomorphic(masm());
  Handle<Code> code = GetCodeWithFlags(flags, "CompileLoadPreMonomorphic");
  PROFILE(isolate(),
          CodeCreateEvent(Logger::LOAD_PREMONOMORPHIC_TAG, *code, 0));
  return code;
}


Handle<Code> PropertyICCompiler::CompileLoadMegamorphic(Code::Flags flags) {
  LoadIC::GenerateMegamorphic(masm());
  Handle<Code> code = GetCodeWithFlags(flags, "CompileLoadMegamorphic");
  PROFILE(isolate(), CodeCreateEvent(Logger::LOAD_MEGAMORPHIC_TAG, *code, 0));
  return code;
}


Handle<Code> PropertyICCompiler::CompileStoreInitialize(Code::Flags flags) {
  StoreIC::GenerateInitialize(masm());
  Handle<Code> code = GetCodeWithFlags(flags, "CompileStoreInitialize");
  PROFILE(isolate(), CodeCreateEvent(Logger::STORE_INITIALIZE_TAG, *code, 0));
  return code;
}


Handle<Code> PropertyICCompiler::CompileStorePreMonomorphic(Code::Flags flags) {
  StoreIC::GeneratePreMonomorphic(masm());
  Handle<Code> code = GetCodeWithFlags(flags, "CompileStorePreMonomorphic");
  PROFILE(isolate(),
          CodeCreateEvent(Logger::STORE_PREMONOMORPHIC_TAG, *code, 0));
  return code;
}


Handle<Code> PropertyICCompiler::CompileStoreGeneric(Code::Flags flags) {
  ExtraICState extra_state = Code::ExtractExtraICStateFromFlags(flags);
  StrictMode strict_mode = StoreIC::GetStrictMode(extra_state);
  GenerateRuntimeSetProperty(masm(), strict_mode);
  Handle<Code> code = GetCodeWithFlags(flags, "CompileStoreGeneric");
  PROFILE(isolate(), CodeCreateEvent(Logger::STORE_GENERIC_TAG, *code, 0));
  return code;
}


Handle<Code> PropertyICCompiler::CompileStoreMegamorphic(Code::Flags flags) {
  StoreIC::GenerateMegamorphic(masm());
  Handle<Code> code = GetCodeWithFlags(flags, "CompileStoreMegamorphic");
  PROFILE(isolate(), CodeCreateEvent(Logger::STORE_MEGAMORPHIC_TAG, *code, 0));
  return code;
}


#define __ ACCESS_MASM(masm())


Register NamedLoadHandlerCompiler::FrontendHeader(Register object_reg,
                                                  Handle<Name> name,
                                                  Label* miss) {
  PrototypeCheckType check_type = CHECK_ALL_MAPS;
  int function_index = -1;
  if (type()->Is(HeapType::String())) {
    function_index = Context::STRING_FUNCTION_INDEX;
  } else if (type()->Is(HeapType::Symbol())) {
    function_index = Context::SYMBOL_FUNCTION_INDEX;
  } else if (type()->Is(HeapType::Number())) {
    function_index = Context::NUMBER_FUNCTION_INDEX;
  } else if (type()->Is(HeapType::Boolean())) {
    function_index = Context::BOOLEAN_FUNCTION_INDEX;
  } else {
    check_type = SKIP_RECEIVER;
  }

  if (check_type == CHECK_ALL_MAPS) {
    GenerateDirectLoadGlobalFunctionPrototype(masm(), function_index,
                                              scratch1(), miss);
    Object* function = isolate()->native_context()->get(function_index);
    Object* prototype = JSFunction::cast(function)->instance_prototype();
    set_type_for_object(handle(prototype, isolate()));
    object_reg = scratch1();
  }

  // Check that the maps starting from the prototype haven't changed.
  return CheckPrototypes(object_reg, scratch1(), scratch2(), scratch3(), name,
                         miss, check_type);
}


// Frontend for store uses the name register. It has to be restored before a
// miss.
Register NamedStoreHandlerCompiler::FrontendHeader(Register object_reg,
                                                   Handle<Name> name,
                                                   Label* miss) {
  return CheckPrototypes(object_reg, this->name(), scratch1(), scratch2(), name,
                         miss, SKIP_RECEIVER);
}


bool PropertyICCompiler::IncludesNumberType(TypeHandleList* types) {
  for (int i = 0; i < types->length(); ++i) {
    if (types->at(i)->Is(HeapType::Number())) return true;
  }
  return false;
}


Register PropertyHandlerCompiler::Frontend(Register object_reg,
                                           Handle<Name> name) {
  Label miss;
  Register reg = FrontendHeader(object_reg, name, &miss);
  FrontendFooter(name, &miss);
  return reg;
}


void PropertyHandlerCompiler::NonexistentFrontendHeader(Handle<Name> name,
                                                        Label* miss,
                                                        Register scratch1,
                                                        Register scratch2) {
  Register holder_reg;
  Handle<Map> last_map;
  if (holder().is_null()) {
    holder_reg = receiver();
    last_map = IC::TypeToMap(*type(), isolate());
    // If |type| has null as its prototype, |holder()| is
    // Handle<JSObject>::null().
    DCHECK(last_map->prototype() == isolate()->heap()->null_value());
  } else {
    holder_reg = FrontendHeader(receiver(), name, miss);
    last_map = handle(holder()->map());
  }

  if (last_map->is_dictionary_map()) {
    if (last_map->IsJSGlobalObjectMap()) {
      Handle<JSGlobalObject> global =
          holder().is_null()
              ? Handle<JSGlobalObject>::cast(type()->AsConstant()->Value())
              : Handle<JSGlobalObject>::cast(holder());
      GenerateCheckPropertyCell(masm(), global, name, scratch1, miss);
    } else {
      if (!name->IsUniqueName()) {
        DCHECK(name->IsString());
        name = factory()->InternalizeString(Handle<String>::cast(name));
      }
      DCHECK(holder().is_null() ||
             holder()->property_dictionary()->FindEntry(name) ==
                 NameDictionary::kNotFound);
      GenerateDictionaryNegativeLookup(masm(), miss, holder_reg, name, scratch1,
                                       scratch2);
    }
  }
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadField(Handle<Name> name,
                                                        FieldIndex field) {
  Register reg = Frontend(receiver(), name);
  __ Move(receiver(), reg);
  LoadFieldStub stub(isolate(), field);
  GenerateTailCall(masm(), stub.GetCode());
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadConstant(Handle<Name> name,
                                                           int constant_index) {
  Register reg = Frontend(receiver(), name);
  __ Move(receiver(), reg);
  LoadConstantStub stub(isolate(), constant_index);
  GenerateTailCall(masm(), stub.GetCode());
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadNonexistent(
    Handle<Name> name) {
  Label miss;
  NonexistentFrontendHeader(name, &miss, scratch2(), scratch3());
  GenerateLoadConstant(isolate()->factory()->undefined_value());
  FrontendFooter(name, &miss);
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadCallback(
    Handle<Name> name, Handle<ExecutableAccessorInfo> callback) {
  Register reg = Frontend(receiver(), name);
  GenerateLoadCallback(reg, callback);
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadCallback(
    Handle<Name> name, const CallOptimization& call_optimization) {
  DCHECK(call_optimization.is_simple_api_call());
  Frontend(receiver(), name);
  Handle<Map> receiver_map = IC::TypeToMap(*type(), isolate());
  GenerateFastApiCall(masm(), call_optimization, receiver_map, receiver(),
                      scratch1(), false, 0, NULL);
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadInterceptor(
    LookupIterator* it) {
  // So far the most popular follow ups for interceptor loads are FIELD and
  // ExecutableAccessorInfo, so inline only them. Other cases may be added
  // later.
  bool inline_followup = it->state() == LookupIterator::PROPERTY;
  if (inline_followup) {
    switch (it->property_kind()) {
      case LookupIterator::DATA:
        inline_followup = it->property_details().type() == FIELD;
        break;
      case LookupIterator::ACCESSOR: {
        Handle<Object> accessors = it->GetAccessors();
        inline_followup = accessors->IsExecutableAccessorInfo();
        if (!inline_followup) break;
        Handle<ExecutableAccessorInfo> info =
            Handle<ExecutableAccessorInfo>::cast(accessors);
        inline_followup = info->getter() != NULL &&
                          ExecutableAccessorInfo::IsCompatibleReceiverType(
                              isolate(), info, type());
      }
    }
  }

  Register reg = Frontend(receiver(), it->name());
  if (inline_followup) {
    // TODO(368): Compile in the whole chain: all the interceptors in
    // prototypes and ultimate answer.
    GenerateLoadInterceptorWithFollowup(it, reg);
  } else {
    GenerateLoadInterceptor(reg);
  }
  return GetCode(kind(), Code::FAST, it->name());
}


void NamedLoadHandlerCompiler::GenerateLoadPostInterceptor(
    LookupIterator* it, Register interceptor_reg) {
  Handle<JSObject> real_named_property_holder(it->GetHolder<JSObject>());

  set_type_for_object(holder());
  set_holder(real_named_property_holder);
  Register reg = Frontend(interceptor_reg, it->name());

  switch (it->property_kind()) {
    case LookupIterator::DATA: {
      DCHECK_EQ(FIELD, it->property_details().type());
      __ Move(receiver(), reg);
      LoadFieldStub stub(isolate(), it->GetFieldIndex());
      GenerateTailCall(masm(), stub.GetCode());
      break;
    }
    case LookupIterator::ACCESSOR:
      Handle<ExecutableAccessorInfo> info =
          Handle<ExecutableAccessorInfo>::cast(it->GetAccessors());
      DCHECK_NE(NULL, info->getter());
      GenerateLoadCallback(reg, info);
  }
}


Handle<Code> PropertyICCompiler::CompileMonomorphic(Handle<HeapType> type,
                                                    Handle<Code> handler,
                                                    Handle<Name> name,
                                                    IcCheckType check) {
  TypeHandleList types(1);
  CodeHandleList handlers(1);
  types.Add(type);
  handlers.Add(handler);
  Code::StubType stub_type = handler->type();
  return CompilePolymorphic(&types, &handlers, name, stub_type, check);
}


Handle<Code> NamedLoadHandlerCompiler::CompileLoadViaGetter(
    Handle<Name> name, Handle<JSFunction> getter) {
  Frontend(receiver(), name);
  GenerateLoadViaGetter(masm(), type(), receiver(), getter);
  return GetCode(kind(), Code::FAST, name);
}


// TODO(verwaest): Cleanup. holder() is actually the receiver.
Handle<Code> NamedStoreHandlerCompiler::CompileStoreTransition(
    Handle<Map> transition, Handle<Name> name) {
  Label miss, slow;

  // Ensure no transitions to deprecated maps are followed.
  __ CheckMapDeprecated(transition, scratch1(), &miss);

  // Check that we are allowed to write this.
  bool is_nonexistent = holder()->map() == transition->GetBackPointer();
  if (is_nonexistent) {
    // Find the top object.
    Handle<JSObject> last;
    PrototypeIterator iter(isolate(), holder());
    while (!iter.IsAtEnd()) {
      last = Handle<JSObject>::cast(PrototypeIterator::GetCurrent(iter));
      iter.Advance();
    }
    if (!last.is_null()) set_holder(last);
    NonexistentFrontendHeader(name, &miss, scratch1(), scratch2());
  } else {
    FrontendHeader(receiver(), name, &miss);
    DCHECK(holder()->HasFastProperties());
  }

  GenerateStoreTransition(transition, name, receiver(), this->name(), value(),
                          scratch1(), scratch2(), scratch3(), &miss, &slow);

  GenerateRestoreName(&miss, name);
  TailCallBuiltin(masm(), MissBuiltin(kind()));

  GenerateRestoreName(&slow, name);
  TailCallBuiltin(masm(), SlowBuiltin(kind()));
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedStoreHandlerCompiler::CompileStoreField(LookupIterator* it) {
  Label miss;
  GenerateStoreField(it, value(), &miss);
  __ bind(&miss);
  TailCallBuiltin(masm(), MissBuiltin(kind()));
  return GetCode(kind(), Code::FAST, it->name());
}


Handle<Code> NamedStoreHandlerCompiler::CompileStoreViaSetter(
    Handle<JSObject> object, Handle<Name> name, Handle<JSFunction> setter) {
  Frontend(receiver(), name);
  GenerateStoreViaSetter(masm(), type(), receiver(), setter);

  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> NamedStoreHandlerCompiler::CompileStoreCallback(
    Handle<JSObject> object, Handle<Name> name,
    const CallOptimization& call_optimization) {
  Frontend(receiver(), name);
  Register values[] = {value()};
  GenerateFastApiCall(masm(), call_optimization, handle(object->map()),
                      receiver(), scratch1(), true, 1, values);
  return GetCode(kind(), Code::FAST, name);
}


Handle<Code> PropertyICCompiler::CompileKeyedStoreMonomorphic(
    Handle<Map> receiver_map, KeyedAccessStoreMode store_mode) {
  ElementsKind elements_kind = receiver_map->elements_kind();
  bool is_jsarray = receiver_map->instance_type() == JS_ARRAY_TYPE;
  Handle<Code> stub;
  if (receiver_map->has_fast_elements() ||
      receiver_map->has_external_array_elements() ||
      receiver_map->has_fixed_typed_array_elements()) {
    stub = StoreFastElementStub(isolate(), is_jsarray, elements_kind,
                                store_mode).GetCode();
  } else {
    stub = StoreElementStub(isolate(), is_jsarray, elements_kind, store_mode)
               .GetCode();
  }

  __ DispatchMap(receiver(), scratch1(), receiver_map, stub, DO_SMI_CHECK);

  TailCallBuiltin(masm(), Builtins::kKeyedStoreIC_Miss);

  return GetCode(kind(), Code::NORMAL, factory()->empty_string());
}


#undef __


Handle<Code> PropertyICCompiler::GetCode(Code::Kind kind, Code::StubType type,
                                         Handle<Name> name,
                                         InlineCacheState state) {
  Code::Flags flags =
      Code::ComputeFlags(kind, state, extra_ic_state_, type, cache_holder());
  Handle<Code> code = GetCodeWithFlags(flags, name);
  IC::RegisterWeakMapDependency(code);
  PROFILE(isolate(), CodeCreateEvent(log_kind(code), *code, *name));
  return code;
}


Handle<Code> PropertyHandlerCompiler::GetCode(Code::Kind kind,
                                              Code::StubType type,
                                              Handle<Name> name) {
  Code::Flags flags = Code::ComputeHandlerFlags(kind, type, cache_holder());
  Handle<Code> code = GetCodeWithFlags(flags, name);
  PROFILE(isolate(), CodeCreateEvent(Logger::STUB_TAG, *code, *name));
  return code;
}


void ElementHandlerCompiler::CompileElementHandlers(
    MapHandleList* receiver_maps, CodeHandleList* handlers) {
  for (int i = 0; i < receiver_maps->length(); ++i) {
    Handle<Map> receiver_map = receiver_maps->at(i);
    Handle<Code> cached_stub;

    if ((receiver_map->instance_type() & kNotStringTag) == 0) {
      cached_stub = isolate()->builtins()->KeyedLoadIC_String();
    } else if (receiver_map->instance_type() < FIRST_JS_RECEIVER_TYPE) {
      cached_stub = isolate()->builtins()->KeyedLoadIC_Slow();
    } else {
      bool is_js_array = receiver_map->instance_type() == JS_ARRAY_TYPE;
      ElementsKind elements_kind = receiver_map->elements_kind();

      if (IsFastElementsKind(elements_kind) ||
          IsExternalArrayElementsKind(elements_kind) ||
          IsFixedTypedArrayElementsKind(elements_kind)) {
        cached_stub = LoadFastElementStub(isolate(), is_js_array, elements_kind)
                          .GetCode();
      } else if (elements_kind == SLOPPY_ARGUMENTS_ELEMENTS) {
        cached_stub = isolate()->builtins()->KeyedLoadIC_SloppyArguments();
      } else {
        DCHECK(elements_kind == DICTIONARY_ELEMENTS);
        cached_stub = LoadDictionaryElementStub(isolate()).GetCode();
      }
    }

    handlers->Add(cached_stub);
  }
}


Handle<Code> PropertyICCompiler::CompileKeyedStorePolymorphic(
    MapHandleList* receiver_maps, KeyedAccessStoreMode store_mode) {
  // Collect MONOMORPHIC stubs for all |receiver_maps|.
  CodeHandleList handlers(receiver_maps->length());
  MapHandleList transitioned_maps(receiver_maps->length());
  for (int i = 0; i < receiver_maps->length(); ++i) {
    Handle<Map> receiver_map(receiver_maps->at(i));
    Handle<Code> cached_stub;
    Handle<Map> transitioned_map =
        receiver_map->FindTransitionedMap(receiver_maps);

    // TODO(mvstanton): The code below is doing pessimistic elements
    // transitions. I would like to stop doing that and rely on Allocation Site
    // Tracking to do a better job of ensuring the data types are what they need
    // to be. Not all the elements are in place yet, pessimistic elements
    // transitions are still important for performance.
    bool is_js_array = receiver_map->instance_type() == JS_ARRAY_TYPE;
    ElementsKind elements_kind = receiver_map->elements_kind();
    if (!transitioned_map.is_null()) {
      cached_stub =
          ElementsTransitionAndStoreStub(isolate(), elements_kind,
                                         transitioned_map->elements_kind(),
                                         is_js_array, store_mode).GetCode();
    } else if (receiver_map->instance_type() < FIRST_JS_RECEIVER_TYPE) {
      cached_stub = isolate()->builtins()->KeyedStoreIC_Slow();
    } else {
      if (receiver_map->has_fast_elements() ||
          receiver_map->has_external_array_elements() ||
          receiver_map->has_fixed_typed_array_elements()) {
        cached_stub = StoreFastElementStub(isolate(), is_js_array,
                                           elements_kind, store_mode).GetCode();
      } else {
        cached_stub = StoreElementStub(isolate(), is_js_array, elements_kind,
                                       store_mode).GetCode();
      }
    }
    DCHECK(!cached_stub.is_null());
    handlers.Add(cached_stub);
    transitioned_maps.Add(transitioned_map);
  }

  Handle<Code> code = CompileKeyedStorePolymorphic(receiver_maps, &handlers,
                                                   &transitioned_maps);
  isolate()->counters()->keyed_store_polymorphic_stubs()->Increment();
  PROFILE(isolate(), CodeCreateEvent(log_kind(code), *code, 0));
  return code;
}


void ElementHandlerCompiler::GenerateStoreDictionaryElement(
    MacroAssembler* masm) {
  KeyedStoreIC::GenerateSlow(masm);
}


}
}  // namespace v8::internal
