// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/code-stub-assembler.h"
#include "src/factory-inl.h"
#include "src/objects/property-descriptor-object.h"
#include "src/objects/shared-function-info.h"

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// ES6 section 19.1 Object Objects

typedef compiler::Node Node;

class ObjectBuiltinsAssembler : public CodeStubAssembler {
 public:
  explicit ObjectBuiltinsAssembler(compiler::CodeAssemblerState* state)
      : CodeStubAssembler(state) {}

 protected:
  void ReturnToStringFormat(Node* context, Node* string);
  void AddToDictionaryIf(Node* condition, Node* name_dictionary,
                         Handle<Name> name, Node* value, Label* bailout);
  Node* FromPropertyDescriptor(Node* context, Node* desc);
  Node* FromPropertyDetails(Node* context, Node* raw_value, Node* details,
                            Label* if_bailout);
  Node* ConstructAccessorDescriptor(Node* context, Node* getter, Node* setter,
                                    Node* enumerable, Node* configurable);
  Node* ConstructDataDescriptor(Node* context, Node* value, Node* writable,
                                Node* enumerable, Node* configurable);
  Node* GetAccessorOrUndefined(Node* accessor, Label* if_bailout);

  void GetOwnValuesOrEntries(Node* context, Node* object, bool get_entries);

  void OnlyHasSimpleProperties(Node* map, Label* if_slow);

  void FastGetOwnValuesOrEntries(Node* context, Node* object,
                                 Variable* var_values_or_entries,
                                 Label* if_fast, Label* if_slow,
                                 Label* if_empty_array, bool get_entries);

  Node* FinalyzeKeysOrValuesJSArray(Node* context, Node* values_or_entries,
                                    Node* initial_size, Node* size,
                                    Node* array_map, Label* if_empty);
};

void ObjectBuiltinsAssembler::ReturnToStringFormat(Node* context,
                                                   Node* string) {
  Node* lhs = StringConstant("[object ");
  Node* rhs = StringConstant("]");

  Callable callable =
      CodeFactory::StringAdd(isolate(), STRING_ADD_CHECK_NONE, NOT_TENURED);

  Return(CallStub(callable, context, CallStub(callable, context, lhs, string),
                  rhs));
}

Node* ObjectBuiltinsAssembler::ConstructAccessorDescriptor(Node* context,
                                                           Node* getter,
                                                           Node* setter,
                                                           Node* enumerable,
                                                           Node* configurable) {
  Node* native_context = LoadNativeContext(context);
  Node* map = LoadContextElement(
      native_context, Context::ACCESSOR_PROPERTY_DESCRIPTOR_MAP_INDEX);
  Node* js_desc = AllocateJSObjectFromMap(map);

  StoreObjectFieldNoWriteBarrier(
      js_desc, JSAccessorPropertyDescriptor::kGetOffset, getter);
  StoreObjectFieldNoWriteBarrier(
      js_desc, JSAccessorPropertyDescriptor::kSetOffset, setter);
  StoreObjectFieldNoWriteBarrier(
      js_desc, JSAccessorPropertyDescriptor::kEnumerableOffset,
      SelectBooleanConstant(enumerable));
  StoreObjectFieldNoWriteBarrier(
      js_desc, JSAccessorPropertyDescriptor::kConfigurableOffset,
      SelectBooleanConstant(configurable));

  return js_desc;
}

Node* ObjectBuiltinsAssembler::ConstructDataDescriptor(Node* context,
                                                       Node* value,
                                                       Node* writable,
                                                       Node* enumerable,
                                                       Node* configurable) {
  Node* native_context = LoadNativeContext(context);
  Node* map = LoadContextElement(native_context,
                                 Context::DATA_PROPERTY_DESCRIPTOR_MAP_INDEX);
  Node* js_desc = AllocateJSObjectFromMap(map);

  StoreObjectFieldNoWriteBarrier(js_desc,
                                 JSDataPropertyDescriptor::kValueOffset, value);
  StoreObjectFieldNoWriteBarrier(js_desc,
                                 JSDataPropertyDescriptor::kWritableOffset,
                                 SelectBooleanConstant(writable));
  StoreObjectFieldNoWriteBarrier(js_desc,
                                 JSDataPropertyDescriptor::kEnumerableOffset,
                                 SelectBooleanConstant(enumerable));
  StoreObjectFieldNoWriteBarrier(js_desc,
                                 JSDataPropertyDescriptor::kConfigurableOffset,
                                 SelectBooleanConstant(configurable));

  return js_desc;
}

void ObjectBuiltinsAssembler::GetOwnValuesOrEntries(Node* context, Node* object,
                                                    bool get_entries) {
  object = CallBuiltin(Builtins::kToObject, context, object);

  VARIABLE(var_values_or_entries, MachineRepresentation::kTagged);
  Label if_fast(this, Label::kDeferred), if_slow(this),
      if_empty_array(this, Label::kDeferred);
  Node* native_context = LoadNativeContext(context);
  Node* array_map = LoadJSArrayElementsMap(PACKED_ELEMENTS, native_context);

  FastGetOwnValuesOrEntries(context, object, &var_values_or_entries, &if_fast,
                            &if_slow, &if_empty_array, get_entries);

  BIND(&if_fast);
  Return(var_values_or_entries.value());

  BIND(&if_slow);
  {
    Node* keys = CallRuntime(Runtime::kGetOwnPropertyKeys, context, object,
                             SmiConstant(SKIP_SYMBOLS));
    CSA_ASSERT(this, IsJSArray(keys));
    Node* key_elements = LoadObjectField(keys, JSArray::kElementsOffset);
    Node* initial_size = LoadAndUntagObjectField(keys, JSArray::kLengthOffset);
    CSA_ASSERT(this, IntPtrGreaterThanOrEqual(initial_size, IntPtrConstant(0)));
    Node* values_or_entries = AllocateFixedArray(PACKED_ELEMENTS, initial_size);

    // The keys array include unenumerable property keys
    // and we should exclude it from the array.
    // But if do so, the array has extra capacity and illegal area.
    // So we fill the array with the-hole.
    FillFixedArrayWithValue(PACKED_ELEMENTS, values_or_entries,
                            IntPtrConstant(0), initial_size,
                            Heap::kTheHoleValueRootIndex);

    VARIABLE(var_index, MachineType::PointerRepresentation(),
             IntPtrConstant(0));
    VARIABLE(var_property_count, MachineType::PointerRepresentation(),
             IntPtrConstant(0));
    Variable* vars[] = {&var_index, &var_property_count};
    Label loop(this, 2, vars), after_loop(this), loop_exit(this),
        loop_condition(this);

    // We dont use BuildFastLoop.
    // Instead, we use hand-written loop
    // because of we need to use 'continue' functionality.
    Branch(IntPtrEqual(var_index.value(), initial_size), &after_loop, &loop);
    BIND(&loop);
    {
      Node* next_key = LoadFixedArrayElement(key_elements, var_index.value());
      Node* desc = CallRuntime(Runtime::kGetOwnPropertyDescriptor, context,
                               object, next_key);
      GotoIf(IsNullOrUndefined(desc), &loop_condition);
      Node* flags = LoadAndUntagToWord32ObjectField(
          desc, PropertyDescriptorObject::kFlagsOffset);
      Node* has_enumerable =
          IsSetWord32<PropertyDescriptorObject::HasEnumerableBit>(flags);
      Node* is_enumerable =
          IsSetWord32<PropertyDescriptorObject::IsEnumerableBit>(flags);
      Node* is_not_enumerable =
          Word32And(has_enumerable, Word32Not(is_enumerable));
      GotoIf(is_not_enumerable, &loop_condition);

      Node* value =
          CallRuntime(Runtime::kGetProperty, context, object, next_key);

      if (get_entries) {
        Node* array = nullptr;
        Node* elements = nullptr;
        std::tie(array, elements) = AllocateUninitializedJSArrayWithElements(
            PACKED_ELEMENTS, array_map, SmiConstant(2), nullptr,
            IntPtrConstant(2));
        StoreFixedArrayElement(elements, 0, next_key);
        StoreFixedArrayElement(elements, 1, value);
        value = array;
      }

      StoreFixedArrayElement(values_or_entries, var_property_count.value(),
                             value);
      Increment(&var_property_count, 1, INTPTR_PARAMETERS);
      Goto(&loop_condition);

      BIND(&loop_condition);
      {
        Increment(&var_index, 1, INTPTR_PARAMETERS);
        Branch(IntPtrEqual(var_index.value(), initial_size), &after_loop,
               &loop);
      }
    }
    BIND(&after_loop);
    {
      Label if_enumerables_exists(this);
      Branch(WordEqual(var_property_count.value(), IntPtrConstant(0)),
             &if_empty_array, &if_enumerables_exists);

      BIND(&if_enumerables_exists);
      {
        Node* array = FinalyzeKeysOrValuesJSArray(
            context, values_or_entries, initial_size,
            var_property_count.value(), array_map, &if_empty_array);
        Return(array);
      }
    }
  }

  BIND(&if_empty_array);
  {
    Node* empty_array =
        AllocateJSArray(PACKED_ELEMENTS, array_map, SmiConstant(0),
                        SmiConstant(0), nullptr, SMI_PARAMETERS);
    Return(empty_array);
  }
}

void ObjectBuiltinsAssembler::OnlyHasSimpleProperties(Node* map,
                                                      Label* if_slow) {
  Node* instance_type = LoadMapInstanceType(map);
  Node* is_string_wrapper_elements_kind = IsStringWrapperElementsKind(map);
  Node* is_special_receiver_instance_type =
      IsSpecialReceiverInstanceType(instance_type);
  Node* has_hidden_prototype = HasHiddenPrototype(map);
  Node* is_dictionary_map = IsDictionaryMap(map);

  GotoIf(is_string_wrapper_elements_kind, if_slow);
  GotoIf(is_special_receiver_instance_type, if_slow);
  GotoIf(has_hidden_prototype, if_slow);
  GotoIf(is_dictionary_map, if_slow);
}

void ObjectBuiltinsAssembler::FastGetOwnValuesOrEntries(
    Node* context, Node* object, Variable* var_values_or_entries,
    Label* if_fast, Label* if_slow, Label* if_empty_array, bool get_entries) {
  Node* map = LoadMap(object);
  GotoIfNot(IsJSObjectMap(map), if_slow);
  OnlyHasSimpleProperties(map, if_slow);

  Node* native_context = LoadNativeContext(context);
  Node* array_map = LoadJSArrayElementsMap(PACKED_ELEMENTS, native_context);
  Node* bit_field3 = LoadMapBitField3(map);
  Node* elements = LoadElements(object);
  Node* object_descriptors = LoadMapDescriptors(map);
  Node* number_of_own_descriptors =
      DecodeWord32<Map::NumberOfOwnDescriptorsBits>(bit_field3);
  Node* number_of_own_elements =
      CallRuntime(Runtime::kGetObjectElementsCount, context, object);

  VARIABLE(var_property_count, MachineType::PointerRepresentation(),
           IntPtrConstant(0));
  Label if_elements(this), if_properties(this), done(this);

  Node* initial_size =
      IntPtrAdd(number_of_own_descriptors, SmiUntag(number_of_own_elements));
  GotoIf(WordEqual(initial_size, IntPtrConstant(0)), if_empty_array);

  Node* values_or_entries =
      AllocateFixedArray(PACKED_ELEMENTS, initial_size, INTPTR_PARAMETERS);

  // If object has elements that type is a Dictionary,
  // we can't get a precise element count (always too big capacity).
  // So we fill invalid zone with the-hole.
  FillFixedArrayWithValue(PACKED_ELEMENTS, values_or_entries, IntPtrConstant(0),
                          initial_size, Heap::kTheHoleValueRootIndex);

  // We dont use BuildFastLoop.
  // Instead, we use hand-written loop
  // because of we need to use 'continue' functionality.
  Branch(WordEqual(elements, LoadRoot(Heap::kEmptyFixedArrayRootIndex)),
         &if_properties, &if_elements);

  BIND(&if_elements);
  {
    Node* result_array = nullptr;
    if (get_entries) {
      result_array = CallRuntime(Runtime::kCollectObjectEntries, context,
                                 object, values_or_entries);
    } else {
      result_array = CallRuntime(Runtime::kCollectObjectValues, context, object,
                                 values_or_entries);
    }
    CSA_ASSERT(this, IsJSArray(result_array));
    Node* result_vector =
        LoadObjectField(result_array, JSArray::kElementsOffset);
    Node* result = LoadFixedArrayElement(result_vector, 0);
    Node* count = SmiUntag(LoadFixedArrayElement(result_vector, 1));
    var_property_count.Bind(count);
    BranchIfToBooleanIsTrue(result, &if_properties, if_empty_array);
  }

  BIND(&if_properties);
  {
    VARIABLE(var_index, MachineType::PointerRepresentation(),
             IntPtrConstant(0));
    VARIABLE(var_property_value, MachineRepresentation::kTagged,
             UndefinedConstant());
    Variable* vars[] = {&var_index, &var_property_count, &var_property_value};
    Label loop(this, 3, vars), after_loop(this), loop_condition(this);
    Branch(IntPtrEqual(var_index.value(), number_of_own_descriptors),
           &after_loop, &loop);
    BIND(&loop);
    {
      // Map must be loaded each loop header to get newest map object.
      map = LoadMap(object);
      object_descriptors = LoadMapDescriptors(map);

      Node* next_key =
          DescriptorArrayGetKey(object_descriptors, var_index.value());
      Node* key_map = LoadMap(next_key);
      Node* key_instance_type = LoadMapInstanceType(key_map);

      // Skip Symbols.
      GotoIf(WordEqual(key_instance_type, IntPtrConstant(SYMBOL_TYPE)),
             &loop_condition);

      {
        Node* instance_type = LoadMapInstanceType(map);
        VARIABLE(var_details, MachineRepresentation::kWord32);
        VARIABLE(var_raw_value, MachineRepresentation::kTagged);
        Label if_found(this), if_not_found(this), if_bailout(this),
            make_pair(this);

        TryGetOwnProperty(context, object, object, map, instance_type, next_key,
                          &if_found, &var_property_value, &var_details,
                          &var_raw_value, &if_not_found, &if_bailout,
                          kCallJSGetter);

        BIND(&if_not_found);
        {
          var_property_value.Bind(UndefinedConstant());
          Goto(&loop_condition);
        }

        BIND(&if_bailout);
        {
          var_property_value.Bind(UndefinedConstant());
          Goto(&make_pair);
        }

        BIND(&if_found);
        {
          Node* details = var_details.value();
          Node* attributes =
              DecodeWord32<PropertyDetails::AttributesField>(details);
          Node* dont_enum = Int32Constant(PropertyAttributes::DONT_ENUM);
          GotoIf(WordNotEqual(WordAnd(attributes, dont_enum), SmiConstant(0)),
                 &loop_condition);
          Goto(&make_pair);
        }

        BIND(&make_pair);
        {
          if (get_entries) {
            Node* array = nullptr;
            Node* elements = nullptr;
            std::tie(array, elements) =
                AllocateUninitializedJSArrayWithElements(
                    PACKED_ELEMENTS, array_map, SmiConstant(2), nullptr,
                    IntPtrConstant(2));
            StoreFixedArrayElement(elements, 0, next_key, SKIP_WRITE_BARRIER);
            StoreFixedArrayElement(elements, 1, var_property_value.value(),
                                   SKIP_WRITE_BARRIER);
            var_property_value.Bind(array);
          }
          StoreFixedArrayElement(values_or_entries, var_property_count.value(),
                                 var_property_value.value());
          Increment(&var_property_count, 1, INTPTR_PARAMETERS);
          Goto(&loop_condition);
        }
      }

      BIND(&loop_condition);
      {
        Increment(&var_index, 1, INTPTR_PARAMETERS);
        Branch(IntPtrEqual(var_index.value(), number_of_own_descriptors),
               &after_loop, &loop);
      }
    }
    BIND(&after_loop);
    Goto(&done);
  }

  BIND(&done);
  {
    Node* array = FinalyzeKeysOrValuesJSArray(
        context, values_or_entries, initial_size, var_property_count.value(),
        array_map, if_empty_array);
    var_values_or_entries->Bind(array);
    Goto(if_fast);
  }
}

Node* ObjectBuiltinsAssembler::FinalyzeKeysOrValuesJSArray(
    Node* context, Node* result, Node* initial_size, Node* size,
    Node* array_map, Label* if_empty) {
  CSA_ASSERT(this, IsFixedArray(result));
  CSA_ASSERT(this, IsJSArrayMap(array_map));

  GotoIf(WordEqual(size, IntPtrConstant(0)), if_empty);
  CSA_ASSERT(this, IntPtrGreaterThanOrEqual(initial_size, size));
  Node* array_length = SmiTag(size);
  Node* array = AllocateUninitializedJSArrayWithoutElements(
      array_map, array_length, nullptr);
  Label if_should_shrink(this), done(this);
  Branch(WordNotEqual(initial_size, size), &if_should_shrink, &done);

  BIND(&if_should_shrink);
  CallRuntime(Runtime::kShrinkFixedArray, context, result, array_length);
  Goto(&done);

  BIND(&done);
  StoreObjectField(array, JSArray::kElementsOffset, result);
  CSA_ASSERT(this, SmiEqual(LoadObjectField(result, FixedArray::kLengthOffset),
                            array_length));
  return array;
}

TF_BUILTIN(ObjectPrototypeToLocaleString, CodeStubAssembler) {
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));
  TNode<Object> receiver = CAST(Parameter(Descriptor::kReceiver));

  PerformStackCheck(context);

  Label if_null_or_undefined(this, Label::kDeferred);
  GotoIf(IsNullOrUndefined(receiver), &if_null_or_undefined);

  TNode<Object> method =
      CAST(GetProperty(context, receiver, factory()->toString_string()));
  Return(CallJS(CodeFactory::Call(isolate()), context, method, receiver));

  BIND(&if_null_or_undefined);
  ThrowTypeError(context, MessageTemplate::kCalledOnNullOrUndefined,
                 "Object.prototype.toLocaleString");
}

TF_BUILTIN(ObjectPrototypeHasOwnProperty, ObjectBuiltinsAssembler) {
  Node* object = Parameter(Descriptor::kReceiver);
  Node* key = Parameter(Descriptor::kKey);
  Node* context = Parameter(Descriptor::kContext);

  Label call_runtime(this), return_true(this), return_false(this),
      to_primitive(this);

  // Smi receivers do not have own properties, just perform ToPrimitive on the
  // key.
  Label if_objectisnotsmi(this);
  Branch(TaggedIsSmi(object), &to_primitive, &if_objectisnotsmi);
  BIND(&if_objectisnotsmi);

  Node* map = LoadMap(object);
  Node* instance_type = LoadMapInstanceType(map);

  {
    VARIABLE(var_index, MachineType::PointerRepresentation());
    VARIABLE(var_unique, MachineRepresentation::kTagged);

    Label if_index(this), if_unique_name(this), if_notunique_name(this);
    TryToName(key, &if_index, &var_index, &if_unique_name, &var_unique,
              &call_runtime, &if_notunique_name);

    BIND(&if_unique_name);
    TryHasOwnProperty(object, map, instance_type, var_unique.value(),
                      &return_true, &return_false, &call_runtime);

    BIND(&if_index);
    {
      // Handle negative keys in the runtime.
      GotoIf(IntPtrLessThan(var_index.value(), IntPtrConstant(0)),
             &call_runtime);
      TryLookupElement(object, map, instance_type, var_index.value(),
                       &return_true, &return_false, &return_false,
                       &call_runtime);
    }

    BIND(&if_notunique_name);
    {
      Label not_in_string_table(this);
      TryInternalizeString(key, &if_index, &var_index, &if_unique_name,
                           &var_unique, &not_in_string_table, &call_runtime);

      BIND(&not_in_string_table);
      {
        // If the string was not found in the string table, then no regular
        // object can have a property with that name, so return |false|.
        // "Special API objects" with interceptors must take the slow path.
        Branch(IsSpecialReceiverInstanceType(instance_type), &call_runtime,
               &return_false);
      }
    }
  }
  BIND(&to_primitive);
  GotoIf(IsNumber(key), &return_false);
  Branch(IsName(key), &return_false, &call_runtime);

  BIND(&return_true);
  Return(TrueConstant());

  BIND(&return_false);
  Return(FalseConstant());

  BIND(&call_runtime);
  Return(CallRuntime(Runtime::kObjectHasOwnProperty, context, object, key));
}

// ES #sec-object.keys
TF_BUILTIN(ObjectKeys, ObjectBuiltinsAssembler) {
  Node* object = Parameter(Descriptor::kObject);
  Node* context = Parameter(Descriptor::kContext);

  VARIABLE(var_length, MachineRepresentation::kTagged);
  VARIABLE(var_elements, MachineRepresentation::kTagged);
  Label if_empty(this, Label::kDeferred), if_empty_elements(this),
      if_fast(this), if_slow(this, Label::kDeferred), if_join(this);

  // Check if the {object} has a usable enum cache.
  GotoIf(TaggedIsSmi(object), &if_slow);
  Node* object_map = LoadMap(object);
  Node* object_bit_field3 = LoadMapBitField3(object_map);
  Node* object_enum_length =
      DecodeWordFromWord32<Map::EnumLengthBits>(object_bit_field3);
  GotoIf(
      WordEqual(object_enum_length, IntPtrConstant(kInvalidEnumCacheSentinel)),
      &if_slow);

  // Ensure that the {object} doesn't have any elements.
  CSA_ASSERT(this, IsJSObjectMap(object_map));
  Node* object_elements = LoadElements(object);
  GotoIf(IsEmptyFixedArray(object_elements), &if_empty_elements);
  Branch(IsEmptySlowElementDictionary(object_elements), &if_empty_elements,
         &if_slow);

  // Check whether there are enumerable properties.
  BIND(&if_empty_elements);
  Branch(WordEqual(object_enum_length, IntPtrConstant(0)), &if_empty, &if_fast);

  BIND(&if_fast);
  {
    // The {object} has a usable enum cache, use that.
    Node* object_descriptors = LoadMapDescriptors(object_map);
    Node* object_enum_cache =
        LoadObjectField(object_descriptors, DescriptorArray::kEnumCacheOffset);
    Node* object_enum_keys =
        LoadObjectField(object_enum_cache, EnumCache::kKeysOffset);

    // Allocate a JSArray and copy the elements from the {object_enum_keys}.
    Node* array = nullptr;
    Node* elements = nullptr;
    Node* native_context = LoadNativeContext(context);
    Node* array_map = LoadJSArrayElementsMap(PACKED_ELEMENTS, native_context);
    Node* array_length = SmiTag(object_enum_length);
    std::tie(array, elements) = AllocateUninitializedJSArrayWithElements(
        PACKED_ELEMENTS, array_map, array_length, nullptr, object_enum_length,
        INTPTR_PARAMETERS);
    CopyFixedArrayElements(PACKED_ELEMENTS, object_enum_keys, elements,
                           object_enum_length, SKIP_WRITE_BARRIER);
    Return(array);
  }

  BIND(&if_empty);
  {
    // The {object} doesn't have any enumerable keys.
    var_length.Bind(SmiConstant(0));
    var_elements.Bind(EmptyFixedArrayConstant());
    Goto(&if_join);
  }

  BIND(&if_slow);
  {
    // Let the runtime compute the elements.
    Node* elements = CallRuntime(Runtime::kObjectKeys, context, object);
    var_length.Bind(LoadObjectField(elements, FixedArray::kLengthOffset));
    var_elements.Bind(elements);
    Goto(&if_join);
  }

  BIND(&if_join);
  {
    // Wrap the elements into a proper JSArray and return that.
    Node* native_context = LoadNativeContext(context);
    Node* array_map = LoadJSArrayElementsMap(PACKED_ELEMENTS, native_context);
    Node* array = AllocateUninitializedJSArrayWithoutElements(
        array_map, var_length.value(), nullptr);
    StoreObjectFieldNoWriteBarrier(array, JSArray::kElementsOffset,
                                   var_elements.value());
    Return(array);
  }
}

TF_BUILTIN(ObjectValues, ObjectBuiltinsAssembler) {
  Node* object = Parameter(Descriptor::kObject);
  Node* context = Parameter(Descriptor::kContext);
  GetOwnValuesOrEntries(context, object, false);
}

TF_BUILTIN(ObjectEntries, ObjectBuiltinsAssembler) {
  Node* object = Parameter(Descriptor::kObject);
  Node* context = Parameter(Descriptor::kContext);
  GetOwnValuesOrEntries(context, object, true);
}

// ES #sec-object.prototype.isprototypeof
TF_BUILTIN(ObjectPrototypeIsPrototypeOf, ObjectBuiltinsAssembler) {
  Node* receiver = Parameter(Descriptor::kReceiver);
  Node* value = Parameter(Descriptor::kValue);
  Node* context = Parameter(Descriptor::kContext);
  Label if_receiverisnullorundefined(this, Label::kDeferred),
      if_valueisnotreceiver(this, Label::kDeferred);

  // We only check whether {value} is a Smi here, so that the
  // prototype chain walk below can safely access the {value}s
  // map. We don't rule out Primitive {value}s, since all of
  // them have null as their prototype, so the chain walk below
  // immediately aborts and returns false anyways.
  GotoIf(TaggedIsSmi(value), &if_valueisnotreceiver);

  // Check if {receiver} is either null or undefined and in that case,
  // invoke the ToObject builtin, which raises the appropriate error.
  // Otherwise we don't need to invoke ToObject, since {receiver} is
  // either already a JSReceiver, in which case ToObject is a no-op,
  // or it's a Primitive and ToObject would allocate a fresh JSValue
  // wrapper, which wouldn't be identical to any existing JSReceiver
  // found in the prototype chain of {value}, hence it will return
  // false no matter if we search for the Primitive {receiver} or
  // a newly allocated JSValue wrapper for {receiver}.
  GotoIf(IsNull(receiver), &if_receiverisnullorundefined);
  GotoIf(IsUndefined(receiver), &if_receiverisnullorundefined);

  // Loop through the prototype chain looking for the {receiver}.
  Return(HasInPrototypeChain(context, value, receiver));

  BIND(&if_receiverisnullorundefined);
  {
    // If {value} is a primitive HeapObject, we need to return
    // false instead of throwing an exception per order of the
    // steps in the specification, so check that first here.
    GotoIfNot(IsJSReceiver(value), &if_valueisnotreceiver);

    // Simulate the ToObject invocation on {receiver}.
    CallBuiltin(Builtins::kToObject, context, receiver);
    Unreachable();
  }

  BIND(&if_valueisnotreceiver);
  Return(FalseConstant());
}

// ES #sec-object.prototype.tostring
TF_BUILTIN(ObjectPrototypeToString, ObjectBuiltinsAssembler) {
  Label checkstringtag(this), if_apiobject(this, Label::kDeferred),
      if_arguments(this), if_array(this), if_boolean(this), if_date(this),
      if_error(this), if_function(this), if_number(this, Label::kDeferred),
      if_object(this), if_primitive(this), if_proxy(this, Label::kDeferred),
      if_regexp(this), if_string(this), if_symbol(this, Label::kDeferred),
      if_value(this), if_bigint(this, Label::kDeferred);

  Node* receiver = Parameter(Descriptor::kReceiver);
  Node* context = Parameter(Descriptor::kContext);

  // This is arranged to check the likely cases first.
  VARIABLE(var_default, MachineRepresentation::kTagged);
  VARIABLE(var_holder, MachineRepresentation::kTagged, receiver);
  GotoIf(TaggedIsSmi(receiver), &if_number);
  Node* receiver_map = LoadMap(receiver);
  Node* receiver_instance_type = LoadMapInstanceType(receiver_map);
  GotoIf(IsPrimitiveInstanceType(receiver_instance_type), &if_primitive);
  const struct {
    InstanceType value;
    Label* label;
  } kJumpTable[] = {{JS_OBJECT_TYPE, &if_object},
                    {JS_ARRAY_TYPE, &if_array},
                    {JS_FUNCTION_TYPE, &if_function},
                    {JS_REGEXP_TYPE, &if_regexp},
                    {JS_ARGUMENTS_TYPE, &if_arguments},
                    {JS_DATE_TYPE, &if_date},
                    {JS_BOUND_FUNCTION_TYPE, &if_function},
                    {JS_API_OBJECT_TYPE, &if_apiobject},
                    {JS_SPECIAL_API_OBJECT_TYPE, &if_apiobject},
                    {JS_PROXY_TYPE, &if_proxy},
                    {JS_ERROR_TYPE, &if_error},
                    {JS_VALUE_TYPE, &if_value}};
  size_t const kNumCases = arraysize(kJumpTable);
  Label* case_labels[kNumCases];
  int32_t case_values[kNumCases];
  for (size_t i = 0; i < kNumCases; ++i) {
    case_labels[i] = kJumpTable[i].label;
    case_values[i] = kJumpTable[i].value;
  }
  Switch(receiver_instance_type, &if_object, case_values, case_labels,
         arraysize(case_values));

  BIND(&if_apiobject);
  {
    // Lookup the @@toStringTag property on the {receiver}.
    VARIABLE(var_tag, MachineRepresentation::kTagged,
             GetProperty(context, receiver,
                         isolate()->factory()->to_string_tag_symbol()));
    Label if_tagisnotstring(this), if_tagisstring(this);
    GotoIf(TaggedIsSmi(var_tag.value()), &if_tagisnotstring);
    Branch(IsString(var_tag.value()), &if_tagisstring, &if_tagisnotstring);
    BIND(&if_tagisnotstring);
    {
      var_tag.Bind(
          CallStub(Builtins::CallableFor(isolate(), Builtins::kClassOf),
                   context, receiver));
      Goto(&if_tagisstring);
    }
    BIND(&if_tagisstring);
    ReturnToStringFormat(context, var_tag.value());
  }

  BIND(&if_arguments);
  {
    var_default.Bind(LoadRoot(Heap::karguments_to_stringRootIndex));
    Goto(&checkstringtag);
  }

  BIND(&if_array);
  {
    var_default.Bind(LoadRoot(Heap::karray_to_stringRootIndex));
    Goto(&checkstringtag);
  }

  BIND(&if_boolean);
  {
    Node* native_context = LoadNativeContext(context);
    Node* boolean_constructor =
        LoadContextElement(native_context, Context::BOOLEAN_FUNCTION_INDEX);
    Node* boolean_initial_map = LoadObjectField(
        boolean_constructor, JSFunction::kPrototypeOrInitialMapOffset);
    Node* boolean_prototype =
        LoadObjectField(boolean_initial_map, Map::kPrototypeOffset);
    var_default.Bind(LoadRoot(Heap::kboolean_to_stringRootIndex));
    var_holder.Bind(boolean_prototype);
    Goto(&checkstringtag);
  }

  BIND(&if_date);
  {
    var_default.Bind(LoadRoot(Heap::kdate_to_stringRootIndex));
    Goto(&checkstringtag);
  }

  BIND(&if_error);
  {
    var_default.Bind(LoadRoot(Heap::kerror_to_stringRootIndex));
    Goto(&checkstringtag);
  }

  BIND(&if_function);
  {
    var_default.Bind(LoadRoot(Heap::kfunction_to_stringRootIndex));
    Goto(&checkstringtag);
  }

  BIND(&if_number);
  {
    Node* native_context = LoadNativeContext(context);
    Node* number_constructor =
        LoadContextElement(native_context, Context::NUMBER_FUNCTION_INDEX);
    Node* number_initial_map = LoadObjectField(
        number_constructor, JSFunction::kPrototypeOrInitialMapOffset);
    Node* number_prototype =
        LoadObjectField(number_initial_map, Map::kPrototypeOffset);
    var_default.Bind(LoadRoot(Heap::knumber_to_stringRootIndex));
    var_holder.Bind(number_prototype);
    Goto(&checkstringtag);
  }

  BIND(&if_object);
  {
    CSA_ASSERT(this, IsJSReceiver(receiver));
    var_default.Bind(LoadRoot(Heap::kobject_to_stringRootIndex));
    Goto(&checkstringtag);
  }

  BIND(&if_primitive);
  {
    Label return_undefined(this);

    GotoIf(IsStringInstanceType(receiver_instance_type), &if_string);
    GotoIf(IsBigIntInstanceType(receiver_instance_type), &if_bigint);
    GotoIf(IsBooleanMap(receiver_map), &if_boolean);
    GotoIf(IsHeapNumberMap(receiver_map), &if_number);
    GotoIf(IsSymbolMap(receiver_map), &if_symbol);
    GotoIf(IsUndefined(receiver), &return_undefined);
    CSA_ASSERT(this, IsNull(receiver));
    Return(LoadRoot(Heap::knull_to_stringRootIndex));

    BIND(&return_undefined);
    Return(LoadRoot(Heap::kundefined_to_stringRootIndex));
  }

  BIND(&if_proxy);
  {
    // If {receiver} is a proxy for a JSArray, we default to "[object Array]",
    // otherwise we default to "[object Object]" or "[object Function]" here,
    // depending on whether the {receiver} is callable. The order matters here,
    // i.e. we need to execute the %ArrayIsArray check before the [[Get]] below,
    // as the exception is observable.
    Node* receiver_is_array =
        CallRuntime(Runtime::kArrayIsArray, context, receiver);
    Node* builtin_tag = SelectTaggedConstant<Object>(
        IsTrue(receiver_is_array), LoadRoot(Heap::kArray_stringRootIndex),
        SelectTaggedConstant<Object>(IsCallableMap(receiver_map),
                                     LoadRoot(Heap::kFunction_stringRootIndex),
                                     LoadRoot(Heap::kObject_stringRootIndex)));

    // Lookup the @@toStringTag property on the {receiver}.
    VARIABLE(var_tag, MachineRepresentation::kTagged,
             GetProperty(context, receiver,
                         isolate()->factory()->to_string_tag_symbol()));
    Label if_tagisnotstring(this), if_tagisstring(this);
    GotoIf(TaggedIsSmi(var_tag.value()), &if_tagisnotstring);
    Branch(IsString(var_tag.value()), &if_tagisstring, &if_tagisnotstring);
    BIND(&if_tagisnotstring);
    {
      var_tag.Bind(builtin_tag);
      Goto(&if_tagisstring);
    }
    BIND(&if_tagisstring);
    ReturnToStringFormat(context, var_tag.value());
  }

  BIND(&if_regexp);
  {
    var_default.Bind(LoadRoot(Heap::kregexp_to_stringRootIndex));
    Goto(&checkstringtag);
  }

  BIND(&if_string);
  {
    Node* native_context = LoadNativeContext(context);
    Node* string_constructor =
        LoadContextElement(native_context, Context::STRING_FUNCTION_INDEX);
    Node* string_initial_map = LoadObjectField(
        string_constructor, JSFunction::kPrototypeOrInitialMapOffset);
    Node* string_prototype =
        LoadObjectField(string_initial_map, Map::kPrototypeOffset);
    var_default.Bind(LoadRoot(Heap::kstring_to_stringRootIndex));
    var_holder.Bind(string_prototype);
    Goto(&checkstringtag);
  }

  BIND(&if_symbol);
  {
    Node* native_context = LoadNativeContext(context);
    Node* symbol_constructor =
        LoadContextElement(native_context, Context::SYMBOL_FUNCTION_INDEX);
    Node* symbol_initial_map = LoadObjectField(
        symbol_constructor, JSFunction::kPrototypeOrInitialMapOffset);
    Node* symbol_prototype =
        LoadObjectField(symbol_initial_map, Map::kPrototypeOffset);
    var_default.Bind(LoadRoot(Heap::kobject_to_stringRootIndex));
    var_holder.Bind(symbol_prototype);
    Goto(&checkstringtag);
  }

  BIND(&if_bigint);
  {
    Node* native_context = LoadNativeContext(context);
    Node* bigint_constructor =
        LoadContextElement(native_context, Context::BIGINT_FUNCTION_INDEX);
    Node* bigint_initial_map = LoadObjectField(
        bigint_constructor, JSFunction::kPrototypeOrInitialMapOffset);
    Node* bigint_prototype =
        LoadObjectField(bigint_initial_map, Map::kPrototypeOffset);
    var_default.Bind(LoadRoot(Heap::kobject_to_stringRootIndex));
    var_holder.Bind(bigint_prototype);
    Goto(&checkstringtag);
  }

  BIND(&if_value);
  {
    Node* receiver_value = LoadJSValueValue(receiver);
    GotoIf(TaggedIsSmi(receiver_value), &if_number);
    Node* receiver_value_map = LoadMap(receiver_value);
    GotoIf(IsHeapNumberMap(receiver_value_map), &if_number);
    GotoIf(IsBooleanMap(receiver_value_map), &if_boolean);
    GotoIf(IsSymbolMap(receiver_value_map), &if_symbol);
    Node* receiver_value_instance_type =
        LoadMapInstanceType(receiver_value_map);
    GotoIf(IsBigIntInstanceType(receiver_value_instance_type), &if_bigint);
    CSA_ASSERT(this, IsStringInstanceType(receiver_value_instance_type));
    Goto(&if_string);
  }

  BIND(&checkstringtag);
  {
    // Check if all relevant maps (including the prototype maps) don't
    // have any interesting symbols (i.e. that none of them have the
    // @@toStringTag property).
    Label loop(this, &var_holder), return_default(this),
        return_generic(this, Label::kDeferred);
    Goto(&loop);
    BIND(&loop);
    {
      Node* holder = var_holder.value();
      GotoIf(IsNull(holder), &return_default);
      Node* holder_map = LoadMap(holder);
      Node* holder_bit_field3 = LoadMapBitField3(holder_map);
      GotoIf(IsSetWord32<Map::MayHaveInterestingSymbolsBit>(holder_bit_field3),
             &return_generic);
      var_holder.Bind(LoadMapPrototype(holder_map));
      Goto(&loop);
    }

    BIND(&return_generic);
    {
      Node* tag = GetProperty(
          context, CallBuiltin(Builtins::kToObject, context, receiver),
          LoadRoot(Heap::kto_string_tag_symbolRootIndex));
      GotoIf(TaggedIsSmi(tag), &return_default);
      GotoIfNot(IsString(tag), &return_default);
      ReturnToStringFormat(context, tag);
    }

    BIND(&return_default);
    Return(var_default.value());
  }
}

// ES6 #sec-object.prototype.valueof
TF_BUILTIN(ObjectPrototypeValueOf, CodeStubAssembler) {
  Node* receiver = Parameter(Descriptor::kReceiver);
  Node* context = Parameter(Descriptor::kContext);

  Return(CallBuiltin(Builtins::kToObject, context, receiver));
}

// ES #sec-object.create
TF_BUILTIN(ObjectCreate, ObjectBuiltinsAssembler) {
  int const kPrototypeArg = 0;
  int const kPropertiesArg = 1;

  Node* argc =
      ChangeInt32ToIntPtr(Parameter(BuiltinDescriptor::kArgumentsCount));
  CodeStubArguments args(this, argc);

  Node* prototype = args.GetOptionalArgumentValue(kPrototypeArg);
  Node* properties = args.GetOptionalArgumentValue(kPropertiesArg);
  Node* context = Parameter(BuiltinDescriptor::kContext);

  Label call_runtime(this, Label::kDeferred), prototype_valid(this),
      no_properties(this);
  {
    Comment("Argument 1 check: prototype");
    GotoIf(IsNull(prototype), &prototype_valid);
    BranchIfJSReceiver(prototype, &prototype_valid, &call_runtime);
  }

  BIND(&prototype_valid);
  {
    Comment("Argument 2 check: properties");
    // Check that we have a simple object
    GotoIf(TaggedIsSmi(properties), &call_runtime);
    // Undefined implies no properties.
    GotoIf(IsUndefined(properties), &no_properties);
    Node* properties_map = LoadMap(properties);
    GotoIf(IsSpecialReceiverMap(properties_map), &call_runtime);
    // Stay on the fast path only if there are no elements.
    GotoIfNot(WordEqual(LoadElements(properties),
                        LoadRoot(Heap::kEmptyFixedArrayRootIndex)),
              &call_runtime);
    // Handle dictionary objects or fast objects with properties in runtime.
    Node* bit_field3 = LoadMapBitField3(properties_map);
    GotoIf(IsSetWord32<Map::IsDictionaryMapBit>(bit_field3), &call_runtime);
    Branch(IsSetWord32<Map::NumberOfOwnDescriptorsBits>(bit_field3),
           &call_runtime, &no_properties);
  }

  // Create a new object with the given prototype.
  BIND(&no_properties);
  {
    VARIABLE(map, MachineRepresentation::kTagged);
    VARIABLE(properties, MachineRepresentation::kTagged);
    Label non_null_proto(this), instantiate_map(this), good(this);

    Branch(IsNull(prototype), &good, &non_null_proto);

    BIND(&good);
    {
      map.Bind(LoadContextElement(
          context, Context::SLOW_OBJECT_WITH_NULL_PROTOTYPE_MAP));
      properties.Bind(AllocateNameDictionary(NameDictionary::kInitialCapacity));
      Goto(&instantiate_map);
    }

    BIND(&non_null_proto);
    {
      properties.Bind(EmptyFixedArrayConstant());
      Node* object_function =
          LoadContextElement(context, Context::OBJECT_FUNCTION_INDEX);
      Node* object_function_map = LoadObjectField(
          object_function, JSFunction::kPrototypeOrInitialMapOffset);
      map.Bind(object_function_map);
      GotoIf(WordEqual(prototype, LoadMapPrototype(map.value())),
             &instantiate_map);
      // Try loading the prototype info.
      Node* prototype_info =
          LoadMapPrototypeInfo(LoadMap(prototype), &call_runtime);
      Comment("Load ObjectCreateMap from PrototypeInfo");
      Node* weak_cell =
          LoadObjectField(prototype_info, PrototypeInfo::kObjectCreateMap);
      GotoIf(IsUndefined(weak_cell), &call_runtime);
      map.Bind(LoadWeakCellValue(weak_cell, &call_runtime));
      Goto(&instantiate_map);
    }

    BIND(&instantiate_map);
    {
      Node* instance = AllocateJSObjectFromMap(map.value(), properties.value());
      args.PopAndReturn(instance);
    }
  }

  BIND(&call_runtime);
  {
    Node* result =
        CallRuntime(Runtime::kObjectCreate, context, prototype, properties);
    args.PopAndReturn(result);
  }
}

// ES #sec-object.is
TF_BUILTIN(ObjectIs, ObjectBuiltinsAssembler) {
  Node* const left = Parameter(Descriptor::kLeft);
  Node* const right = Parameter(Descriptor::kRight);

  Label return_true(this), return_false(this);
  BranchIfSameValue(left, right, &return_true, &return_false);

  BIND(&return_true);
  Return(TrueConstant());

  BIND(&return_false);
  Return(FalseConstant());
}

TF_BUILTIN(CreateIterResultObject, ObjectBuiltinsAssembler) {
  Node* const value = Parameter(Descriptor::kValue);
  Node* const done = Parameter(Descriptor::kDone);
  Node* const context = Parameter(Descriptor::kContext);

  Node* const native_context = LoadNativeContext(context);
  Node* const map =
      LoadContextElement(native_context, Context::ITERATOR_RESULT_MAP_INDEX);

  Node* const result = AllocateJSObjectFromMap(map);

  StoreObjectFieldNoWriteBarrier(result, JSIteratorResult::kValueOffset, value);
  StoreObjectFieldNoWriteBarrier(result, JSIteratorResult::kDoneOffset, done);

  Return(result);
}

TF_BUILTIN(HasProperty, ObjectBuiltinsAssembler) {
  Node* key = Parameter(Descriptor::kKey);
  Node* object = Parameter(Descriptor::kObject);
  Node* context = Parameter(Descriptor::kContext);

  Return(HasProperty(object, key, context, kHasProperty));
}

TF_BUILTIN(InstanceOf, ObjectBuiltinsAssembler) {
  Node* object = Parameter(Descriptor::kLeft);
  Node* callable = Parameter(Descriptor::kRight);
  Node* context = Parameter(Descriptor::kContext);

  Return(InstanceOf(object, callable, context));
}

// ES6 section 7.3.19 OrdinaryHasInstance ( C, O )
TF_BUILTIN(OrdinaryHasInstance, ObjectBuiltinsAssembler) {
  Node* constructor = Parameter(Descriptor::kLeft);
  Node* object = Parameter(Descriptor::kRight);
  Node* context = Parameter(Descriptor::kContext);

  Return(OrdinaryHasInstance(context, constructor, object));
}

TF_BUILTIN(GetSuperConstructor, ObjectBuiltinsAssembler) {
  Node* object = Parameter(Descriptor::kObject);
  Node* context = Parameter(Descriptor::kContext);

  Return(GetSuperConstructor(object, context));
}

TF_BUILTIN(CreateGeneratorObject, ObjectBuiltinsAssembler) {
  Node* closure = Parameter(Descriptor::kClosure);
  Node* receiver = Parameter(Descriptor::kReceiver);
  Node* context = Parameter(Descriptor::kContext);

  // Get the initial map from the function, jumping to the runtime if we don't
  // have one.
  Label runtime(this);
  GotoIfNot(IsFunctionWithPrototypeSlotMap(LoadMap(closure)), &runtime);
  Node* maybe_map =
      LoadObjectField(closure, JSFunction::kPrototypeOrInitialMapOffset);
  GotoIf(DoesntHaveInstanceType(maybe_map, MAP_TYPE), &runtime);

  Node* shared =
      LoadObjectField(closure, JSFunction::kSharedFunctionInfoOffset);
  Node* bytecode_array =
      LoadObjectField(shared, SharedFunctionInfo::kFunctionDataOffset);
  Node* frame_size = ChangeInt32ToIntPtr(LoadObjectField(
      bytecode_array, BytecodeArray::kFrameSizeOffset, MachineType::Int32()));
  Node* size = WordSar(frame_size, IntPtrConstant(kPointerSizeLog2));
  Node* register_file = AllocateFixedArray(HOLEY_ELEMENTS, size);
  FillFixedArrayWithValue(HOLEY_ELEMENTS, register_file, IntPtrConstant(0),
                          size, Heap::kUndefinedValueRootIndex);
  // TODO(cbruni): support start_offset to avoid double initialization.
  Node* result = AllocateJSObjectFromMap(maybe_map, nullptr, nullptr, kNone,
                                         kWithSlackTracking);
  StoreObjectFieldNoWriteBarrier(result, JSGeneratorObject::kFunctionOffset,
                                 closure);
  StoreObjectFieldNoWriteBarrier(result, JSGeneratorObject::kContextOffset,
                                 context);
  StoreObjectFieldNoWriteBarrier(result, JSGeneratorObject::kReceiverOffset,
                                 receiver);
  StoreObjectFieldNoWriteBarrier(result, JSGeneratorObject::kRegisterFileOffset,
                                 register_file);
  Node* executing = SmiConstant(JSGeneratorObject::kGeneratorExecuting);
  StoreObjectFieldNoWriteBarrier(result, JSGeneratorObject::kContinuationOffset,
                                 executing);
  Return(result);

  BIND(&runtime);
  {
    Return(CallRuntime(Runtime::kCreateJSGeneratorObject, context, closure,
                       receiver));
  }
}

// ES6 section 19.1.2.7 Object.getOwnPropertyDescriptor ( O, P )
TF_BUILTIN(ObjectGetOwnPropertyDescriptor, ObjectBuiltinsAssembler) {
  Node* argc = Parameter(BuiltinDescriptor::kArgumentsCount);
  Node* context = Parameter(BuiltinDescriptor::kContext);
  CSA_ASSERT(this, IsUndefined(Parameter(BuiltinDescriptor::kNewTarget)));

  CodeStubArguments args(this, ChangeInt32ToIntPtr(argc));
  Node* object = args.GetOptionalArgumentValue(0);
  Node* key = args.GetOptionalArgumentValue(1);

  // 1. Let obj be ? ToObject(O).
  object = CallBuiltin(Builtins::kToObject, context, object);

  // 2. Let key be ? ToPropertyKey(P).
  key = ToName(context, key);

  // 3. Let desc be ? obj.[[GetOwnProperty]](key).
  Label if_keyisindex(this), if_iskeyunique(this),
      call_runtime(this, Label::kDeferred),
      return_undefined(this, Label::kDeferred), if_notunique_name(this);
  Node* map = LoadMap(object);
  Node* instance_type = LoadMapInstanceType(map);
  GotoIf(Int32LessThanOrEqual(instance_type,
                              Int32Constant(LAST_SPECIAL_RECEIVER_TYPE)),
         &call_runtime);
  {
    VARIABLE(var_index, MachineType::PointerRepresentation(),
             IntPtrConstant(0));
    VARIABLE(var_name, MachineRepresentation::kTagged);

    TryToName(key, &if_keyisindex, &var_index, &if_iskeyunique, &var_name,
              &call_runtime, &if_notunique_name);

    BIND(&if_notunique_name);
    {
      Label not_in_string_table(this);
      TryInternalizeString(key, &if_keyisindex, &var_index, &if_iskeyunique,
                           &var_name, &not_in_string_table, &call_runtime);

      BIND(&not_in_string_table);
      {
        // If the string was not found in the string table, then no regular
        // object can have a property with that name, so return |undefined|.
        Goto(&return_undefined);
      }
    }

    BIND(&if_iskeyunique);
    {
      Label if_found_value(this), return_empty(this), if_not_found(this);

      VARIABLE(var_value, MachineRepresentation::kTagged);
      VARIABLE(var_details, MachineRepresentation::kWord32);
      VARIABLE(var_raw_value, MachineRepresentation::kTagged);

      TryGetOwnProperty(context, object, object, map, instance_type,
                        var_name.value(), &if_found_value, &var_value,
                        &var_details, &var_raw_value, &return_empty,
                        &if_not_found, kReturnAccessorPair);

      BIND(&if_found_value);
      // 4. Return FromPropertyDescriptor(desc).
      Node* js_desc = FromPropertyDetails(context, var_value.value(),
                                          var_details.value(), &call_runtime);
      args.PopAndReturn(js_desc);

      BIND(&return_empty);
      var_value.Bind(UndefinedConstant());
      args.PopAndReturn(UndefinedConstant());

      BIND(&if_not_found);
      Goto(&call_runtime);
    }
  }

  BIND(&if_keyisindex);
  Goto(&call_runtime);

  BIND(&call_runtime);
  {
    Node* desc =
        CallRuntime(Runtime::kGetOwnPropertyDescriptor, context, object, key);

    GotoIf(IsUndefined(desc), &return_undefined);

    CSA_ASSERT(this, IsFixedArray(desc));

    // 4. Return FromPropertyDescriptor(desc).
    Node* js_desc = FromPropertyDescriptor(context, desc);
    args.PopAndReturn(js_desc);
  }
  BIND(&return_undefined);
  args.PopAndReturn(UndefinedConstant());
}

void ObjectBuiltinsAssembler::AddToDictionaryIf(Node* condition,
                                                Node* name_dictionary,
                                                Handle<Name> name, Node* value,
                                                Label* bailout) {
  Label done(this);
  GotoIfNot(condition, &done);

  Add<NameDictionary>(name_dictionary, HeapConstant(name), value, bailout);
  Goto(&done);

  BIND(&done);
}

Node* ObjectBuiltinsAssembler::FromPropertyDescriptor(Node* context,
                                                      Node* desc) {
  VARIABLE(js_descriptor, MachineRepresentation::kTagged);

  Node* flags = LoadAndUntagToWord32ObjectField(
      desc, PropertyDescriptorObject::kFlagsOffset);

  Node* has_flags =
      Word32And(flags, Int32Constant(PropertyDescriptorObject::kHasMask));

  Label if_accessor_desc(this), if_data_desc(this), if_generic_desc(this),
      return_desc(this);
  GotoIf(
      Word32Equal(has_flags,
                  Int32Constant(
                      PropertyDescriptorObject::kRegularAccessorPropertyBits)),
      &if_accessor_desc);
  GotoIf(Word32Equal(
             has_flags,
             Int32Constant(PropertyDescriptorObject::kRegularDataPropertyBits)),
         &if_data_desc);
  Goto(&if_generic_desc);

  BIND(&if_accessor_desc);
  {
    js_descriptor.Bind(ConstructAccessorDescriptor(
        context, LoadObjectField(desc, PropertyDescriptorObject::kGetOffset),
        LoadObjectField(desc, PropertyDescriptorObject::kSetOffset),
        IsSetWord32<PropertyDescriptorObject::IsEnumerableBit>(flags),
        IsSetWord32<PropertyDescriptorObject::IsConfigurableBit>(flags)));
    Goto(&return_desc);
  }

  BIND(&if_data_desc);
  {
    js_descriptor.Bind(ConstructDataDescriptor(
        context, LoadObjectField(desc, PropertyDescriptorObject::kValueOffset),
        IsSetWord32<PropertyDescriptorObject::IsWritableBit>(flags),
        IsSetWord32<PropertyDescriptorObject::IsEnumerableBit>(flags),
        IsSetWord32<PropertyDescriptorObject::IsConfigurableBit>(flags)));
    Goto(&return_desc);
  }

  BIND(&if_generic_desc);
  {
    Node* native_context = LoadNativeContext(context);
    Node* map = LoadContextElement(
        native_context, Context::SLOW_OBJECT_WITH_OBJECT_PROTOTYPE_MAP);
    // We want to preallocate the slots for value, writable, get, set,
    // enumerable and configurable - a total of 6
    Node* properties = AllocateNameDictionary(6);
    Node* js_desc = AllocateJSObjectFromMap(map, properties);

    Label bailout(this, Label::kDeferred);

    Factory* factory = isolate()->factory();
    Node* value = LoadObjectField(desc, PropertyDescriptorObject::kValueOffset);
    AddToDictionaryIf(IsNotTheHole(value), properties, factory->value_string(),
                      value, &bailout);
    AddToDictionaryIf(
        IsSetWord32<PropertyDescriptorObject::HasWritableBit>(flags),
        properties, factory->writable_string(),
        SelectBooleanConstant(
            IsSetWord32<PropertyDescriptorObject::IsWritableBit>(flags)),
        &bailout);

    Node* get = LoadObjectField(desc, PropertyDescriptorObject::kGetOffset);
    AddToDictionaryIf(IsNotTheHole(get), properties, factory->get_string(), get,
                      &bailout);
    Node* set = LoadObjectField(desc, PropertyDescriptorObject::kSetOffset);
    AddToDictionaryIf(IsNotTheHole(set), properties, factory->set_string(), set,
                      &bailout);

    AddToDictionaryIf(
        IsSetWord32<PropertyDescriptorObject::HasEnumerableBit>(flags),
        properties, factory->enumerable_string(),
        SelectBooleanConstant(
            IsSetWord32<PropertyDescriptorObject::IsEnumerableBit>(flags)),
        &bailout);
    AddToDictionaryIf(
        IsSetWord32<PropertyDescriptorObject::HasConfigurableBit>(flags),
        properties, factory->configurable_string(),
        SelectBooleanConstant(
            IsSetWord32<PropertyDescriptorObject::IsConfigurableBit>(flags)),
        &bailout);

    js_descriptor.Bind(js_desc);
    Goto(&return_desc);

    BIND(&bailout);
    CSA_ASSERT(this, Int32Constant(0));
    Unreachable();
  }

  BIND(&return_desc);
  return js_descriptor.value();
}

Node* ObjectBuiltinsAssembler::FromPropertyDetails(Node* context,
                                                   Node* raw_value,
                                                   Node* details,
                                                   Label* if_bailout) {
  VARIABLE(js_descriptor, MachineRepresentation::kTagged);

  Label if_accessor_desc(this), if_data_desc(this), return_desc(this);
  BranchIfAccessorPair(raw_value, &if_accessor_desc, &if_data_desc);

  BIND(&if_accessor_desc);
  {
    Node* getter = LoadObjectField(raw_value, AccessorPair::kGetterOffset);
    Node* setter = LoadObjectField(raw_value, AccessorPair::kSetterOffset);
    js_descriptor.Bind(ConstructAccessorDescriptor(
        context, GetAccessorOrUndefined(getter, if_bailout),
        GetAccessorOrUndefined(setter, if_bailout),
        IsNotSetWord32(details, PropertyDetails::kAttributesDontEnumMask),
        IsNotSetWord32(details, PropertyDetails::kAttributesDontDeleteMask)));
    Goto(&return_desc);
  }

  BIND(&if_data_desc);
  {
    js_descriptor.Bind(ConstructDataDescriptor(
        context, raw_value,
        IsNotSetWord32(details, PropertyDetails::kAttributesReadOnlyMask),
        IsNotSetWord32(details, PropertyDetails::kAttributesDontEnumMask),
        IsNotSetWord32(details, PropertyDetails::kAttributesDontDeleteMask)));
    Goto(&return_desc);
  }

  BIND(&return_desc);
  return js_descriptor.value();
}

Node* ObjectBuiltinsAssembler::GetAccessorOrUndefined(Node* accessor,
                                                      Label* if_bailout) {
  Label bind_undefined(this, Label::kDeferred), return_result(this);
  VARIABLE(result, MachineRepresentation::kTagged);

  GotoIf(IsNull(accessor), &bind_undefined);
  result.Bind(accessor);
  Node* map = LoadMap(accessor);
  // TODO(ishell): probe template instantiations cache.
  GotoIf(IsFunctionTemplateInfoMap(map), if_bailout);
  Goto(&return_result);

  BIND(&bind_undefined);
  result.Bind(UndefinedConstant());
  Goto(&return_result);

  BIND(&return_result);
  return result.value();
}
}  // namespace internal
}  // namespace v8
