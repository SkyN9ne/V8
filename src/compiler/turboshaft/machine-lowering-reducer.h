// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_TURBOSHAFT_MACHINE_LOWERING_REDUCER_H_
#define V8_COMPILER_TURBOSHAFT_MACHINE_LOWERING_REDUCER_H_

#include "src/base/v8-fallthrough.h"
#include "src/common/globals.h"
#include "src/compiler/access-builder.h"
#include "src/compiler/globals.h"
#include "src/compiler/simplified-operator.h"
#include "src/compiler/turboshaft/assembler.h"
#include "src/compiler/turboshaft/index.h"
#include "src/compiler/turboshaft/operations.h"
#include "src/compiler/turboshaft/optimization-phase.h"
#include "src/compiler/turboshaft/reducer-traits.h"
#include "src/compiler/turboshaft/representations.h"
#include "src/objects/bigint.h"
#include "src/objects/heap-number.h"
#include "src/objects/oddball.h"
#include "src/utils/utils.h"

namespace v8::internal::compiler::turboshaft {

#include "src/compiler/turboshaft/define-assembler-macros.inc"

struct MachineLoweringReducerArgs {
  Factory* factory;
};

// MachineLoweringReducer, formerly known as EffectControlLinearizer, lowers
// simplified operations to machine operations.
template <typename Next>
class MachineLoweringReducer : public Next {
 public:
  TURBOSHAFT_REDUCER_BOILERPLATE()

  using ArgT =
      base::append_tuple_type<typename Next::ArgT, MachineLoweringReducerArgs>;

  template <typename... Args>
  explicit MachineLoweringReducer(const std::tuple<Args...>& args)
      : Next(args),
        factory_(std::get<MachineLoweringReducerArgs>(args).factory) {}

  bool NeedsHeapObjectCheck(ObjectIsOp::InputAssumptions input_assumptions) {
    // TODO(nicohartmann@): Consider type information once we have that.
    switch (input_assumptions) {
      case ObjectIsOp::InputAssumptions::kNone:
        return true;
      case ObjectIsOp::InputAssumptions::kHeapObject:
      case ObjectIsOp::InputAssumptions::kBigInt:
        return false;
    }
  }

  OpIndex ReduceChangeOrDeopt(OpIndex input, OpIndex frame_state,
                              ChangeOrDeoptOp::Kind kind,
                              CheckForMinusZeroMode minus_zero_mode,
                              const FeedbackSource& feedback) {
    switch (kind) {
      case ChangeOrDeoptOp::Kind::kUint32ToInt32: {
        __ DeoptimizeIf(__ Int32LessThan(input, 0), frame_state,
                        DeoptimizeReason::kLostPrecision, feedback);
        return input;
      }
      case ChangeOrDeoptOp::Kind::kInt64ToInt32: {
        // Int64 is truncated to Int32 implicitly.
        V<Word32> i32 = input;
        __ DeoptimizeIfNot(__ Word64Equal(__ ChangeInt32ToInt64(i32), input),
                           frame_state, DeoptimizeReason::kLostPrecision,
                           feedback);
        return i32;
      }
      case ChangeOrDeoptOp::Kind::kUint64ToInt32: {
        __ DeoptimizeIfNot(
            __ Uint64LessThanOrEqual(input, static_cast<uint64_t>(kMaxInt)),
            frame_state, DeoptimizeReason::kLostPrecision, feedback);
        // Uint64 is truncated to Int32 implicitly
        return input;
      }
      case ChangeOrDeoptOp::Kind::kUint64ToInt64: {
        __ DeoptimizeIfNot(__ Uint64LessThanOrEqual(
                               input, std::numeric_limits<int64_t>::max()),
                           frame_state, DeoptimizeReason::kLostPrecision,
                           feedback);
        return input;
      }
      case ChangeOrDeoptOp::Kind::kFloat64ToInt32: {
        V<Word32> i32 = __ TruncateFloat64ToInt32OverflowUndefined(input);
        __ DeoptimizeIfNot(__ Float64Equal(__ ChangeInt32ToFloat64(i32), input),
                           frame_state, DeoptimizeReason::kLostPrecisionOrNaN,
                           feedback);

        if (minus_zero_mode == CheckForMinusZeroMode::kCheckForMinusZero) {
          // Check if {value} is -0.
          IF_UNLIKELY(__ Word32Equal(i32, 0)) {
            // In case of 0, we need to check the high bits for the IEEE -0
            // pattern.
            V<Word32> check_negative =
                __ Int32LessThan(__ Float64ExtractHighWord32(input), 0);
            __ DeoptimizeIf(check_negative, frame_state,
                            DeoptimizeReason::kMinusZero, feedback);
          }
          END_IF
        }

        return i32;
      }
      case ChangeOrDeoptOp::Kind::kFloat64ToInt64: {
        V<Word64> i64 = __ TruncateFloat64ToInt64OverflowUndefined(input);
        __ DeoptimizeIfNot(__ Float64Equal(__ ChangeInt64ToFloat64(i64), input),
                           frame_state, DeoptimizeReason::kLostPrecisionOrNaN,
                           feedback);

        if (minus_zero_mode == CheckForMinusZeroMode::kCheckForMinusZero) {
          // Check if {value} is -0.
          IF_UNLIKELY(__ Word64Equal(i64, 0)) {
            // In case of 0, we need to check the high bits for the IEEE -0
            // pattern.
            V<Word32> check_negative =
                __ Int32LessThan(__ Float64ExtractHighWord32(input), 0);
            __ DeoptimizeIf(check_negative, frame_state,
                            DeoptimizeReason::kMinusZero, feedback);
          }
          END_IF
        }

        return i64;
      }
    }
    UNREACHABLE();
  }

  V<Word32> ReduceObjectIs(V<Tagged> input, ObjectIsOp::Kind kind,
                           ObjectIsOp::InputAssumptions input_assumptions) {
    switch (kind) {
      case ObjectIsOp::Kind::kBigInt:
      case ObjectIsOp::Kind::kBigInt64: {
        DCHECK_IMPLIES(kind == ObjectIsOp::Kind::kBigInt64, Is64());

        Label<Word32> done(this);

        if (input_assumptions != ObjectIsOp::InputAssumptions::kBigInt) {
          if (NeedsHeapObjectCheck(input_assumptions)) {
            // Check for Smi.
            GOTO_IF(IsSmi(input), done, 0);
          }

          // Check for BigInt.
          V<Tagged> map = __ LoadMapField(input);
          V<Word32> is_bigint_map =
              __ TaggedEqual(map, __ HeapConstant(factory_->bigint_map()));
          GOTO_IF_NOT(is_bigint_map, done, 0);
        }

        if (kind == ObjectIsOp::Kind::kBigInt) {
          GOTO(done, 1);
        } else {
          DCHECK_EQ(kind, ObjectIsOp::Kind::kBigInt64);
          // We have to perform check for BigInt64 range.
          V<Word32> bitfield = __ template LoadField<Word32>(
              input, AccessBuilder::ForBigIntBitfield());
          GOTO_IF(__ Word32Equal(bitfield, 0), done, 1);

          // Length must be 1.
          V<Word32> length_field =
              __ Word32BitwiseAnd(bitfield, BigInt::LengthBits::kMask);
          GOTO_IF_NOT(__ Word32Equal(length_field,
                                     uint32_t{1} << BigInt::LengthBits::kShift),
                      done, 0);

          // Check if it fits in 64 bit signed int.
          V<Word64> lsd = __ template LoadField<Word64>(
              input, AccessBuilder::ForBigIntLeastSignificantDigit64());
          V<Word32> magnitude_check = __ Uint64LessThanOrEqual(
              lsd, std::numeric_limits<int64_t>::max());
          GOTO_IF(magnitude_check, done, 1);

          // The BigInt probably doesn't fit into signed int64. The only
          // exception is int64_t::min. We check for this.
          V<Word32> sign =
              __ Word32BitwiseAnd(bitfield, BigInt::SignBits::kMask);
          V<Word32> sign_check = __ Word32Equal(sign, BigInt::SignBits::kMask);
          GOTO_IF_NOT(sign_check, done, 0);

          V<Word32> min_check =
              __ Word64Equal(lsd, std::numeric_limits<int64_t>::min());
          GOTO_IF(min_check, done, 1);

          GOTO(done, 0);
        }

        BIND(done, result);
        return result;
      }
      case ObjectIsOp::Kind::kCallable:
      case ObjectIsOp::Kind::kConstructor:
      case ObjectIsOp::Kind::kDetectableCallable:
      case ObjectIsOp::Kind::kNonCallable:
      case ObjectIsOp::Kind::kReceiver:
      case ObjectIsOp::Kind::kUndetectable: {
        Label<Word32> done(this);

        // Check for Smi if necessary.
        if (NeedsHeapObjectCheck(input_assumptions)) {
          GOTO_IF(IsSmi(input), done, 0);
        }

        // Load bitfield from map.
        V<Tagged> map = __ LoadMapField(input);
        V<Word32> bitfield =
            __ template LoadField<Word32>(map, AccessBuilder::ForMapBitField());

        V<Word32> check;
        switch (kind) {
          case ObjectIsOp::Kind::kCallable:
            check =
                __ Word32Equal(Map::Bits1::IsCallableBit::kMask,
                               __ Word32BitwiseAnd(
                                   bitfield, Map::Bits1::IsCallableBit::kMask));
            break;
          case ObjectIsOp::Kind::kConstructor:
            check = __ Word32Equal(
                Map::Bits1::IsConstructorBit::kMask,
                __ Word32BitwiseAnd(bitfield,
                                    Map::Bits1::IsConstructorBit::kMask));
            break;
          case ObjectIsOp::Kind::kDetectableCallable:
            check = __ Word32Equal(
                Map::Bits1::IsCallableBit::kMask,
                __ Word32BitwiseAnd(
                    bitfield, (Map::Bits1::IsCallableBit::kMask) |
                                  (Map::Bits1::IsUndetectableBit::kMask)));
            break;
          case ObjectIsOp::Kind::kNonCallable:
            check = __ Word32Equal(
                0, __ Word32BitwiseAnd(bitfield,
                                       Map::Bits1::IsCallableBit::kMask));
            GOTO_IF_NOT(check, done, 0);
            // Fallthrough into receiver check.
            V8_FALLTHROUGH;
          case ObjectIsOp::Kind::kReceiver: {
            static_assert(LAST_TYPE == LAST_JS_RECEIVER_TYPE);
            V<Word32> instance_type = __ template LoadField<Word32>(
                map, AccessBuilder::ForMapInstanceType());
            check =
                __ Uint32LessThanOrEqual(FIRST_JS_RECEIVER_TYPE, instance_type);
            break;
          }
          case ObjectIsOp::Kind::kUndetectable:
            check = __ Word32Equal(
                Map::Bits1::IsUndetectableBit::kMask,
                __ Word32BitwiseAnd(bitfield,
                                    Map::Bits1::IsUndetectableBit::kMask));
            break;
          default:
            UNREACHABLE();
        }
        GOTO(done, check);

        BIND(done, result);
        return result;
      }
      case ObjectIsOp::Kind::kSmi: {
        // If we statically know that this is a heap object, it cannot be a Smi.
        if (!NeedsHeapObjectCheck(input_assumptions)) {
          return __ Word32Constant(0);
        }
        return IsSmi(input);
      }
      case ObjectIsOp::Kind::kNumber: {
        Label<Word32> done(this);

        // Check for Smi if necessary.
        if (NeedsHeapObjectCheck(input_assumptions)) {
          GOTO_IF(IsSmi(input), done, 1);
        }

        V<Tagged> map = __ LoadMapField(input);
        GOTO(done,
             __ TaggedEqual(map, __ HeapConstant(factory_->heap_number_map())));

        BIND(done, result);
        return result;
      }
      case ObjectIsOp::Kind::kSymbol:
      case ObjectIsOp::Kind::kString:
      case ObjectIsOp::Kind::kArrayBufferView: {
        Label<Word32> done(this);

        // Check for Smi if necessary.
        if (NeedsHeapObjectCheck(input_assumptions)) {
          GOTO_IF(IsSmi(input), done, 0);
        }

        // Load instance type from map.
        V<Tagged> map = __ LoadMapField(input);
        V<Word32> instance_type = __ template LoadField<Word32>(
            map, AccessBuilder::ForMapInstanceType());

        V<Word32> check;
        switch (kind) {
          case ObjectIsOp::Kind::kSymbol:
            check = __ Word32Equal(instance_type, SYMBOL_TYPE);
            break;
          case ObjectIsOp::Kind::kString:
            check = __ Uint32LessThan(instance_type, FIRST_NONSTRING_TYPE);
            break;
          case ObjectIsOp::Kind::kArrayBufferView:
            check = __ Uint32LessThan(
                __ Word32Sub(instance_type, FIRST_JS_ARRAY_BUFFER_VIEW_TYPE),
                LAST_JS_ARRAY_BUFFER_VIEW_TYPE -
                    FIRST_JS_ARRAY_BUFFER_VIEW_TYPE + 1);
            break;
          default:
            UNREACHABLE();
        }
        GOTO(done, check);

        BIND(done, result);
        return result;
      }
    }

    UNREACHABLE();
  }

  OpIndex ReduceConvertToObject(
      OpIndex input, ConvertToObjectOp::Kind kind,
      RegisterRepresentation input_rep,
      ConvertToObjectOp::InputInterpretation input_interpretation,
      CheckForMinusZeroMode minus_zero_mode) {
    switch (kind) {
      case ConvertToObjectOp::Kind::kBigInt: {
        DCHECK(Is64());
        DCHECK_EQ(input_rep, RegisterRepresentation::Word64());
        Label<Tagged> done(this);

        // BigInts with value 0 must be of size 0 (canonical form).
        GOTO_IF(__ Word64Equal(input, int64_t{0}), done,
                AllocateBigInt(OpIndex::Invalid(), OpIndex::Invalid()));

        if (input_interpretation ==
            ConvertToObjectOp::InputInterpretation::kSigned) {
          // Shift sign bit into BigInt's sign bit position.
          V<Word32> bitfield = __ Word32BitwiseOr(
              BigInt::LengthBits::encode(1),
              __ Word64ShiftRightLogical(
                  input, static_cast<int64_t>(63 - BigInt::SignBits::kShift)));

          // We use (value XOR (value >> 63)) - (value >> 63) to compute the
          // absolute value, in a branchless fashion.
          V<Word64> sign_mask =
              __ Word64ShiftRightArithmetic(input, int64_t{63});
          V<Word64> absolute_value =
              __ Word64Sub(__ Word64BitwiseXor(input, sign_mask), sign_mask);
          GOTO(done, AllocateBigInt(bitfield, absolute_value));
        } else {
          DCHECK_EQ(input_interpretation,
                    ConvertToObjectOp::InputInterpretation::kUnsigned);
          const auto bitfield = BigInt::LengthBits::encode(1);
          GOTO(done, AllocateBigInt(__ Word32Constant(bitfield), input));
        }
        BIND(done, result);
        return result;
      }
      case ConvertToObjectOp::Kind::kNumber: {
        if (input_rep == RegisterRepresentation::Word32()) {
          switch (input_interpretation) {
            case ConvertToObjectOp::InputInterpretation::kSigned: {
              if (SmiValuesAre32Bits()) {
                return __ SmiTag(input);
              }
              DCHECK(SmiValuesAre31Bits());

              Label<Tagged> done(this);
              Label<> overflow(this);

              SmiTagOrOverflow(input, &overflow, &done);

              if (BIND(overflow)) {
                GOTO(done, AllocateHeapNumberWithValue(
                               __ ChangeInt32ToFloat64(input)));
              }

              BIND(done, result);
              return result;
            }
            case ConvertToObjectOp::InputInterpretation::kUnsigned: {
              Label<Tagged> done(this);

              GOTO_IF(__ Uint32LessThanOrEqual(input, Smi::kMaxValue), done,
                      __ SmiTag(input));
              GOTO(done, AllocateHeapNumberWithValue(
                             __ ChangeUint32ToFloat64(input)));

              BIND(done, result);
              return result;
            }
            case ConvertToObjectOp::InputInterpretation::kCharCode:
            case ConvertToObjectOp::InputInterpretation::kCodePoint:
              UNREACHABLE();
          }
        } else if (input_rep == RegisterRepresentation::Word64()) {
          switch (input_interpretation) {
            case ConvertToObjectOp::InputInterpretation::kSigned: {
              Label<Tagged> done(this);
              Label<> outside_smi_range(this);

              V<Word32> v32 = input;
              V<Word64> v64 = __ ChangeInt32ToInt64(v32);
              GOTO_IF_NOT(__ Word64Equal(v64, input), outside_smi_range);

              if constexpr (SmiValuesAre32Bits()) {
                GOTO(done, __ SmiTag(input));
              } else {
                SmiTagOrOverflow(v32, &outside_smi_range, &done);
              }

              if (BIND(outside_smi_range)) {
                GOTO(done, AllocateHeapNumberWithValue(
                               __ ChangeInt64ToFloat64(input)));
              }

              BIND(done, result);
              return result;
            }
            case ConvertToObjectOp::InputInterpretation::kUnsigned: {
              Label<Tagged> done(this);

              GOTO_IF(__ Uint64LessThanOrEqual(input, Smi::kMaxValue), done,
                      __ SmiTag(input));
              GOTO(done,
                   AllocateHeapNumberWithValue(__ ChangeInt64ToFloat64(input)));

              BIND(done, result);
              return result;
            }
            case ConvertToObjectOp::InputInterpretation::kCharCode:
            case ConvertToObjectOp::InputInterpretation::kCodePoint:
              UNREACHABLE();
          }
        } else {
          DCHECK_EQ(input_rep, RegisterRepresentation::Float64());
          Label<Tagged> done(this);
          Label<> outside_smi_range(this);

          V<Word32> v32 = __ TruncateFloat64ToInt32OverflowUndefined(input);
          GOTO_IF_NOT(__ Float64Equal(input, __ ChangeInt32ToFloat64(v32)),
                      outside_smi_range);

          if (minus_zero_mode == CheckForMinusZeroMode::kCheckForMinusZero) {
            // In case of 0, we need to check the high bits for the IEEE -0
            // pattern.
            IF(__ Word32Equal(v32, 0)) {
              GOTO_IF(__ Int32LessThan(__ Float64ExtractHighWord32(input), 0),
                      outside_smi_range);
            }
            END_IF
          }

          if constexpr (SmiValuesAre32Bits()) {
            GOTO(done, __ SmiTag(v32));
          } else {
            SmiTagOrOverflow(v32, &outside_smi_range, &done);
          }

          if (BIND(outside_smi_range)) {
            GOTO(done, AllocateHeapNumberWithValue(input));
          }

          BIND(done, result);
          return result;
        }
        UNREACHABLE();
        break;
      }
      case ConvertToObjectOp::Kind::kHeapNumber: {
        DCHECK_EQ(input_rep, RegisterRepresentation::Float64());
        DCHECK_EQ(input_interpretation,
                  ConvertToObjectOp::InputInterpretation::kSigned);
        return AllocateHeapNumberWithValue(input);
      }
      case ConvertToObjectOp::Kind::kSmi: {
        DCHECK_EQ(input_rep, RegisterRepresentation::Word32());
        DCHECK_EQ(input_interpretation,
                  ConvertToObjectOp::InputInterpretation::kSigned);
        return __ SmiTag(input);
      }
      case ConvertToObjectOp::Kind::kBoolean: {
        DCHECK_EQ(input_rep, RegisterRepresentation::Word32());
        DCHECK_EQ(input_interpretation,
                  ConvertToObjectOp::InputInterpretation::kSigned);
        Label<Tagged> done(this);

        IF(input) { GOTO(done, __ HeapConstant(factory_->true_value())); }
        ELSE { GOTO(done, __ HeapConstant(factory_->false_value())); }
        END_IF

        BIND(done, result);
        return result;
      }
      case ConvertToObjectOp::Kind::kString: {
        Label<Word32> single_code(this);
        Label<Tagged> done(this);

        if (input_interpretation ==
            ConvertToObjectOp::InputInterpretation::kCharCode) {
          GOTO(single_code, __ Word32BitwiseAnd(input, 0xFFFF));
        } else {
          DCHECK_EQ(input_interpretation,
                    ConvertToObjectOp::InputInterpretation::kCodePoint);
          // Check if the input is a single code unit.
          GOTO_IF_LIKELY(__ Uint32LessThanOrEqual(input, 0xFFFF), single_code,
                         input);

          // Generate surrogate pair string.

          // Convert UTF32 to UTF16 code units and store as a 32 bit word.
          V<Word32> lead_offset = __ Word32Constant(0xD800 - (0x10000 >> 10));

          // lead = (codepoint >> 10) + LEAD_OFFSET
          V<Word32> lead =
              __ Word32Add(__ Word32ShiftRightLogical(input, 10), lead_offset);

          // trail = (codepoint & 0x3FF) + 0xDC00
          V<Word32> trail =
              __ Word32Add(__ Word32BitwiseAnd(input, 0x3FF), 0xDC00);

          // codepoint = (trail << 16) | lead
#if V8_TARGET_BIG_ENDIAN
          V<Word32> code =
              __ Word32BitwiseOr(__ Word32ShiftLeft(lead, 16), trail);
#else
          V<Word32> code =
              __ Word32BitwiseOr(__ Word32ShiftLeft(trail, 16), lead);
#endif

          // Allocate a new SeqTwoByteString for {code}.
          V<Tagged> string =
              __ Allocate(__ IntPtrConstant(SeqTwoByteString::SizeFor(2)),
                          AllocationType::kYoung);
          // Set padding to 0.
          __ Store(string, __ IntPtrConstant(0),
                   StoreOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
                   MemoryRepresentation::TaggedSigned(), kNoWriteBarrier,
                   SeqTwoByteString::SizeFor(2) - kObjectAlignment);
          __ StoreField(string, AccessBuilder::ForMap(),
                        __ HeapConstant(factory_->string_map()));
          __ StoreField(string, AccessBuilder::ForNameRawHashField(),
                        __ Word32Constant(Name::kEmptyHashField));
          __ StoreField(string, AccessBuilder::ForStringLength(),
                        __ Word32Constant(2));
          __ Store(string, code,
                   StoreOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
                   MemoryRepresentation::Uint32(), kNoWriteBarrier,
                   SeqTwoByteString::kHeaderSize);
          GOTO(done, string);
        }

        if (BIND(single_code, code)) {
          // Check if the {code} is a one byte character.
          IF_LIKELY(
              __ Uint32LessThanOrEqual(code, String::kMaxOneByteCharCode)) {
            // Load the isolate wide single character string table.
            OpIndex table =
                __ HeapConstant(factory_->single_character_string_table());

            // Compute the {table} index for {code}.
            V<WordPtr> index = __ ChangeUint32ToUintPtr(code);

            // Load the string for the {code} from the single character string
            // table.
            OpIndex entry = __ LoadElement(
                table, AccessBuilder::ForFixedArrayElement(), index);

            // Use the {entry} from the {table}.
            GOTO(done, entry);
          }
          ELSE {
            // Allocate a new SeqTwoBytesString for {code}.
            V<Tagged> string =
                __ Allocate(__ IntPtrConstant(SeqTwoByteString::SizeFor(1)),
                            AllocationType::kYoung);

            // Set padding to 0.
            __ Store(string, __ IntPtrConstant(0),
                     StoreOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
                     MemoryRepresentation::TaggedSigned(), kNoWriteBarrier,
                     SeqTwoByteString::SizeFor(1) - kObjectAlignment);
            __ StoreField(string, AccessBuilder::ForMap(),
                          __ HeapConstant(factory_->string_map()));
            __ StoreField(string, AccessBuilder::ForNameRawHashField(),
                          __ Word32Constant(Name::kEmptyHashField));
            __ StoreField(string, AccessBuilder::ForStringLength(),
                          __ Word32Constant(1));
            __ Store(string, code,
                     StoreOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
                     MemoryRepresentation::Uint16(), kNoWriteBarrier,
                     SeqTwoByteString::kHeaderSize);
            GOTO(done, string);
          }
          END_IF
        }

        BIND(done, result);
        return result;
      }
    }

    UNREACHABLE();
  }

  OpIndex ReduceConvertObjectToPrimitive(
      OpIndex object, ConvertObjectToPrimitiveOp::Kind kind,
      ConvertObjectToPrimitiveOp::InputAssumptions input_assumptions) {
    switch (kind) {
      case ConvertObjectToPrimitiveOp::Kind::kInt32:
        if (input_assumptions ==
            ConvertObjectToPrimitiveOp::InputAssumptions::kSmi) {
          return __ SmiUntag(object);
        } else {
          DCHECK_EQ(
              input_assumptions,
              ConvertObjectToPrimitiveOp::InputAssumptions::kNumberOrOddball);
          Label<Word32> done(this);

          IF(__ ObjectIsSmi(object)) { GOTO(done, __ SmiUntag(object)); }
          ELSE {
            STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                              Oddball::kToNumberRawOffset);
            V<Float64> value = __ template LoadField<Float64>(
                object, AccessBuilder::ForHeapNumberValue());
            GOTO(done, __ ReversibleFloat64ToInt32(value));
          }
          END_IF

          BIND(done, result);
          return result;
        }
        UNREACHABLE();
      case ConvertObjectToPrimitiveOp::Kind::kInt64:
        if (input_assumptions ==
            ConvertObjectToPrimitiveOp::InputAssumptions::kSmi) {
          return __ ChangeInt32ToInt64(__ SmiUntag(object));
        } else {
          DCHECK_EQ(
              input_assumptions,
              ConvertObjectToPrimitiveOp::InputAssumptions::kNumberOrOddball);
          Label<Word64> done(this);

          IF(__ ObjectIsSmi(object)) {
            GOTO(done, __ ChangeInt32ToInt64(__ SmiUntag(object)));
          }
          ELSE {
            STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                              Oddball::kToNumberRawOffset);
            V<Float64> value = __ template LoadField<Float64>(
                object, AccessBuilder::ForHeapNumberValue());
            GOTO(done, __ ReversibleFloat64ToInt64(value));
          }
          END_IF

          BIND(done, result);
          return result;
        }
        UNREACHABLE();
      case ConvertObjectToPrimitiveOp::Kind::kUint32: {
        DCHECK_EQ(
            input_assumptions,
            ConvertObjectToPrimitiveOp::InputAssumptions::kNumberOrOddball);
        Label<Word32> done(this);

        IF(__ ObjectIsSmi(object)) { GOTO(done, __ SmiUntag(object)); }
        ELSE {
          STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                            Oddball::kToNumberRawOffset);
          V<Float64> value = __ template LoadField<Float64>(
              object, AccessBuilder::ForHeapNumberValue());
          GOTO(done, __ ReversibleFloat64ToUint32(value));
        }
        END_IF

        BIND(done, result);
        return result;
      }
      case ConvertObjectToPrimitiveOp::Kind::kBit:
        DCHECK_EQ(input_assumptions,
                  ConvertObjectToPrimitiveOp::InputAssumptions::kObject);
        return __ TaggedEqual(object, __ HeapConstant(factory_->true_value()));
    }
  }

  OpIndex ReduceConvertObjectToPrimitiveOrDeopt(
      V<Tagged> object, OpIndex frame_state,
      ConvertObjectToPrimitiveOrDeoptOp::ObjectKind from_kind,
      ConvertObjectToPrimitiveOrDeoptOp::PrimitiveKind to_kind,
      CheckForMinusZeroMode minus_zero_mode, const FeedbackSource& feedback) {
    switch (to_kind) {
      case ConvertObjectToPrimitiveOrDeoptOp::PrimitiveKind::kInt32: {
        if (from_kind == ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kSmi) {
          __ DeoptimizeIfNot(__ ObjectIsSmi(object), frame_state,
                             DeoptimizeReason::kNotASmi, feedback);
          return __ SmiUntag(object);
        } else {
          DCHECK_EQ(from_kind,
                    ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kNumber);
          Label<Word32> done(this);

          IF_LIKELY(__ ObjectIsSmi(object)) { GOTO(done, __ SmiUntag(object)); }
          ELSE {
            V<Tagged> map = __ LoadMapField(object);
            __ DeoptimizeIfNot(
                __ TaggedEqual(map,
                               __ HeapConstant(factory_->heap_number_map())),
                frame_state, DeoptimizeReason::kNotAHeapNumber, feedback);
            V<Float64> heap_number_value = __ template LoadField<Float64>(
                object, AccessBuilder::ForHeapNumberValue());

            GOTO(done,
                 __ ChangeFloat64ToInt32OrDeopt(heap_number_value, frame_state,
                                                minus_zero_mode, feedback));
          }
          END_IF

          BIND(done, result);
          return result;
        }
      }
      case ConvertObjectToPrimitiveOrDeoptOp::PrimitiveKind::kInt64: {
        DCHECK_EQ(from_kind,
                  ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kNumber);
        Label<Word64> done(this);

        IF_LIKELY(__ ObjectIsSmi(object)) {
          GOTO(done, __ ChangeInt32ToInt64(__ SmiUntag(object)));
        }
        ELSE {
          V<Tagged> map = __ LoadMapField(object);
          __ DeoptimizeIfNot(
              __ TaggedEqual(map, __ HeapConstant(factory_->heap_number_map())),
              frame_state, DeoptimizeReason::kNotAHeapNumber, feedback);
          V<Float64> heap_number_value = __ template LoadField<Float64>(
              object, AccessBuilder::ForHeapNumberValue());
          GOTO(done,
               __ ChangeFloat64ToInt64OrDeopt(heap_number_value, frame_state,
                                              minus_zero_mode, feedback));
        }
        END_IF

        BIND(done, result);
        return result;
      }
      case ConvertObjectToPrimitiveOrDeoptOp::PrimitiveKind::kFloat64: {
        Label<Float64> done(this);

        // In the Smi case, just convert to int32 and then float64.
        // Otherwise, check heap numberness and load the number.
        IF(__ ObjectIsSmi(object)) {
          GOTO(done, __ ChangeInt32ToFloat64(__ SmiUntag(object)));
        }
        ELSE {
          GOTO(done, ConvertHeapObjectToFloat64OrDeopt(object, frame_state,
                                                       from_kind, feedback));
        }
        END_IF

        BIND(done, result);
        return result;
      }
      case ConvertObjectToPrimitiveOrDeoptOp::PrimitiveKind::kArrayIndex: {
        DCHECK_EQ(
            from_kind,
            ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kNumberOrString);
        Label<WordPtr> done(this);

        IF_LIKELY(__ ObjectIsSmi(object)) {
          // In the Smi case, just convert to intptr_t.
          GOTO(done, __ ChangeInt32ToIntPtr(__ SmiUntag(object)));
        }
        ELSE {
          V<Tagged> map = __ LoadMapField(object);
          IF_LIKELY(__ TaggedEqual(
              map, __ HeapConstant(factory_->heap_number_map()))) {
            V<Float64> heap_number_value = __ template LoadField<Float64>(
                object, AccessBuilder::ForHeapNumberValue());
            // Perform Turbofan's "CheckedFloat64ToIndex"
            {
              if constexpr (Is64()) {
                V<Word64> i64 = __ TruncateFloat64ToInt64OverflowUndefined(
                    heap_number_value);
                // The TruncateKind above means there will be a precision loss
                // in case INT64_MAX input is passed, but that precision loss
                // would not be detected and would not lead to a deoptimization
                // from the first check. But in this case, we'll deopt anyway
                // because of the following checks.
                __ DeoptimizeIfNot(__ Float64Equal(__ ChangeInt64ToFloat64(i64),
                                                   heap_number_value),
                                   frame_state,
                                   DeoptimizeReason::kLostPrecisionOrNaN,
                                   feedback);
                __ DeoptimizeIfNot(
                    __ IntPtrLessThan(i64, kMaxSafeIntegerUint64), frame_state,
                    DeoptimizeReason::kNotAnArrayIndex, feedback);
                __ DeoptimizeIfNot(
                    __ IntPtrLessThan(-kMaxSafeIntegerUint64, i64), frame_state,
                    DeoptimizeReason::kNotAnArrayIndex, feedback);
                GOTO(done, i64);
              } else {
                V<Word32> i32 = __ TruncateFloat64ToInt32OverflowUndefined(
                    heap_number_value);
                __ DeoptimizeIfNot(__ Float64Equal(__ ChangeInt32ToFloat64(i32),
                                                   heap_number_value),
                                   frame_state,
                                   DeoptimizeReason::kLostPrecisionOrNaN,
                                   feedback);
                GOTO(done, i32);
              }
            }
          }
          ELSE {
            V<Word32> instance_type = __ template LoadField<Word32>(
                map, AccessBuilder::ForMapInstanceType());
            __ DeoptimizeIfNot(
                __ Uint32LessThan(instance_type, FIRST_NONSTRING_TYPE),
                frame_state, DeoptimizeReason::kNotAString, feedback);

            // TODO(nicohartmann@): We might introduce a Turboshaft way for
            // constructing call descriptors.
            MachineSignature::Builder builder(__ graph_zone(), 1, 1);
            builder.AddReturn(MachineType::IntPtr());
            builder.AddParam(MachineType::TaggedPointer());
            auto desc = Linkage::GetSimplifiedCDescriptor(__ graph_zone(),
                                                          builder.Build());
            auto ts_desc = TSCallDescriptor::Create(desc, __ graph_zone());
            OpIndex callee = __ ExternalConstant(
                ExternalReference::string_to_array_index_function());
            // NOTE: String::ToArrayIndex() currently returns int32_t.
            V<WordPtr> index =
                __ ChangeInt32ToIntPtr(__ Call(callee, {object}, ts_desc));
            __ DeoptimizeIf(__ WordPtrEqual(index, -1), frame_state,
                            DeoptimizeReason::kNotAnArrayIndex, feedback);
            GOTO(done, index);
          }
          END_IF
        }
        END_IF

        BIND(done, result);
        return result;
      }
    }
    UNREACHABLE();
  }

  OpIndex ReduceNewConsString(OpIndex length, OpIndex first, OpIndex second) {
    // Determine the instance types of {first} and {second}.
    V<Tagged> first_map = __ LoadMapField(first);
    V<Word32> first_type = __ template LoadField<Word32>(
        first_map, AccessBuilder::ForMapInstanceType());
    V<Tagged> second_map = __ LoadMapField(second);
    V<Word32> second_type = __ template LoadField<Word32>(
        second_map, AccessBuilder::ForMapInstanceType());

    Label<Tagged> allocate_string(this);
    // Determine the proper map for the resulting ConsString.
    // If both {first} and {second} are one-byte strings, we
    // create a new ConsOneByteString, otherwise we create a
    // new ConsString instead.
    static_assert(kOneByteStringTag != 0);
    static_assert(kTwoByteStringTag == 0);
    V<Word32> instance_type = __ Word32BitwiseAnd(first_type, second_type);
    V<Word32> encoding =
        __ Word32BitwiseAnd(instance_type, kStringEncodingMask);
    IF(__ Word32Equal(encoding, kTwoByteStringTag)) {
      GOTO(allocate_string, __ HeapConstant(factory_->cons_string_map()));
    }
    ELSE {
      GOTO(allocate_string,
           __ HeapConstant(factory_->cons_one_byte_string_map()));
    }

    // Allocate the resulting ConsString.
    BIND(allocate_string, map);
    V<Tagged> string = __ Allocate(__ IntPtrConstant(ConsString::kSize),
                                   AllocationType::kYoung);
    __ StoreField(string, AccessBuilder::ForMap(), map);
    __ StoreField(string, AccessBuilder::ForNameRawHashField(),
                  __ Word32Constant(Name::kEmptyHashField));
    __ StoreField(string, AccessBuilder::ForStringLength(), length);
    __ StoreField(string, AccessBuilder::ForConsStringFirst(), first);
    __ StoreField(string, AccessBuilder::ForConsStringSecond(), second);
    return string;
  }

  OpIndex ReduceNewArray(V<WordPtr> length, NewArrayOp::Kind kind,
                         AllocationType allocation_type) {
    Label<Tagged> done(this);

    GOTO_IF(__ WordPtrEqual(length, 0), done,
            __ HeapConstant(factory_->empty_fixed_array()));

    // Compute the effective size of the backing store.
    intptr_t size_log2;
    Handle<Map> array_map;
    // TODO(nicohartmann@): Replace ElementAccess by a Turboshaft replacement.
    ElementAccess access;
    V<Any> the_hole_value;
    switch (kind) {
      case NewArrayOp::Kind::kDouble: {
        size_log2 = kDoubleSizeLog2;
        array_map = factory_->fixed_double_array_map();
        access = {kTaggedBase, FixedDoubleArray::kHeaderSize,
                  compiler::Type::NumberOrHole(), MachineType::Float64(),
                  kNoWriteBarrier};
        STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                          Oddball::kToNumberRawOffset);
        the_hole_value = __ template LoadField<Float64>(
            __ HeapConstant(factory_->the_hole_value()),
            AccessBuilder::ForHeapNumberValue());
        break;
      }
      case NewArrayOp::Kind::kObject: {
        size_log2 = kTaggedSizeLog2;
        array_map = factory_->fixed_array_map();
        access = {kTaggedBase, FixedArray::kHeaderSize, compiler::Type::Any(),
                  MachineType::AnyTagged(), kNoWriteBarrier};
        the_hole_value = __ HeapConstant(factory_->the_hole_value());
        break;
      }
    }
    V<WordPtr> size = __ WordPtrAdd(__ WordPtrShiftLeft(length, size_log2),
                                    access.header_size);

    // Allocate the result and initialize the header.
    V<Tagged> array = __ Allocate(size, allocation_type);
    __ StoreField(array, AccessBuilder::ForMap(), __ HeapConstant(array_map));
    __ StoreField(array, AccessBuilder::ForFixedArrayLength(),
                  __ SmiTag(length));

    // Initialize the backing store with holes.
    LoopLabel<WordPtr> loop(this);
    GOTO(loop, intptr_t{0});

    if (BIND(loop, index)) {
      GOTO_IF_NOT_UNLIKELY(__ UintPtrLessThan(index, length), done, array);

      __ StoreElement(array, access, index, the_hole_value);

      // Advance the {index}.
      GOTO(loop, __ WordPtrAdd(index, 1));
    }

    BIND(done, result);
    return result;
  }

  OpIndex ReduceDoubleArrayMinMax(V<Tagged> array,
                                  DoubleArrayMinMaxOp::Kind kind) {
    DCHECK(kind == DoubleArrayMinMaxOp::Kind::kMin ||
           kind == DoubleArrayMinMaxOp::Kind::kMax);
    const bool is_max = kind == DoubleArrayMinMaxOp::Kind::kMax;

    // Iterate the elements and find the result.
    V<Float64> empty_value =
        __ Float64Constant(is_max ? -V8_INFINITY : V8_INFINITY);
    V<WordPtr> array_length =
        __ ChangeInt32ToIntPtr(__ SmiUntag(__ template LoadField<Tagged>(
            array, AccessBuilder::ForJSArrayLength(
                       ElementsKind::PACKED_DOUBLE_ELEMENTS))));
    V<Tagged> elements = __ template LoadField<Tagged>(
        array, AccessBuilder::ForJSObjectElements());

    Label<Float64> done(this);
    LoopLabel<WordPtr, Float64> loop(this);

    GOTO(loop, intptr_t{0}, empty_value);

    if (BIND(loop, index, accumulator)) {
      GOTO_IF_NOT_UNLIKELY(__ UintPtrLessThan(index, array_length), done,
                           accumulator);

      V<Float64> element = __ template LoadElement<Float64>(
          elements, AccessBuilder::ForFixedDoubleArrayElement(), index);

      V<Float64> new_accumulator = is_max ? __ Float64Max(accumulator, element)
                                          : __ Float64Min(accumulator, element);
      GOTO(loop, __ WordPtrAdd(index, 1), new_accumulator);
    }

    BIND(done, result);
    return __ ConvertFloat64ToNumber(result,
                                     CheckForMinusZeroMode::kCheckForMinusZero);
  }

  OpIndex ReduceLoadFieldByIndex(V<Tagged> object, V<Word32> field_index) {
    // Index encoding (see `src/objects/field-index-inl.h`):
    // For efficiency, the LoadByFieldIndex instruction takes an index that is
    // optimized for quick access. If the property is inline, the index is
    // positive. If it's out-of-line, the encoded index is -raw_index - 1 to
    // disambiguate the zero out-of-line index from the zero inobject case.
    // The index itself is shifted up by one bit, the lower-most bit
    // signifying if the field is a mutable double box (1) or not (0).
    V<WordPtr> index = __ ChangeInt32ToIntPtr(field_index);

    Label<> double_field(this);
    Label<Tagged> done(this);

    // Check if field is a mutable double field.
    GOTO_IF_UNLIKELY(__ WordPtrBitwiseAnd(index, 0x1), double_field);

    {
      // The field is a proper Tagged field on {object}. The {index} is
      // shifted to the left by one in the code below.

      // Check if field is in-object or out-of-object.
      IF(__ IntPtrLessThan(index, 0)) {
        // The field is located in the properties backing store of {object}.
        // The {index} is equal to the negated out of property index plus 1.
        V<Tagged> properties = __ template LoadField<Tagged>(
            object, AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer());

        V<WordPtr> out_of_object_index = __ WordPtrSub(0, index);
        V<Tagged> result =
            __ Load(properties, out_of_object_index,
                    LoadOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
                    MemoryRepresentation::AnyTagged(),
                    FixedArray::kHeaderSize - kTaggedSize, kTaggedSizeLog2 - 1);
        GOTO(done, result);
      }
      ELSE {
        // This field is located in the {object} itself.
        V<Tagged> result = __ Load(
            object, index, LoadOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
            MemoryRepresentation::AnyTagged(), JSObject::kHeaderSize,
            kTaggedSizeLog2 - 1);
        GOTO(done, result);
      }
      END_IF
    }

    if (BIND(double_field)) {
      // If field is a Double field, either unboxed in the object on 64 bit
      // architectures, or a mutable HeapNumber.
      V<WordPtr> double_index = __ WordPtrShiftRightArithmetic(index, 1);
      Label<Tagged> loaded_field(this);

      // Check if field is in-object or out-of-object.
      IF(__ IntPtrLessThan(double_index, 0)) {
        V<Tagged> properties = __ template LoadField<Tagged>(
            object, AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer());

        V<WordPtr> out_of_object_index = __ WordPtrSub(0, double_index);
        V<Tagged> result =
            __ Load(properties, out_of_object_index,
                    LoadOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
                    MemoryRepresentation::AnyTagged(),
                    FixedArray::kHeaderSize - kTaggedSize, kTaggedSizeLog2);
        GOTO(loaded_field, result);
      }
      ELSE {
        // The field is located in the {object} itself.
        V<Tagged> result =
            __ Load(object, double_index,
                    LoadOp::Kind::Aligned(BaseTaggedness::kTaggedBase),
                    MemoryRepresentation::AnyTagged(), JSObject::kHeaderSize,
                    kTaggedSizeLog2);
        GOTO(loaded_field, result);
      }
      END_IF

      if (BIND(loaded_field, field)) {
        // We may have transitioned in-place away from double, so check that
        // this is a HeapNumber -- otherwise the load is fine and we don't need
        // to copy anything anyway.
        GOTO_IF(__ ObjectIsSmi(field), done, field);
        V<Tagged> map =
            __ template LoadField<Tagged>(field, AccessBuilder::ForMap());
        GOTO_IF_NOT(
            __ TaggedEqual(map, __ HeapConstant(factory_->heap_number_map())),
            done, field);

        V<Float64> value = __ template LoadField<Float64>(
            field, AccessBuilder::ForHeapNumberValue());
        GOTO(done, AllocateHeapNumberWithValue(value));
      }
    }

    BIND(done, result);
    return result;
  }

  // TODO(nicohartmann@): Remove this once ECL has been fully ported.
  // ECL: ChangeInt64ToSmi(input) ==> MLR: __ SmiTag(input)
  // ECL: ChangeInt32ToSmi(input) ==> MLR: __ SmiTag(input)
  // ECL: ChangeUint32ToSmi(input) ==> MLR: __ SmiTag(input)
  // ECL: ChangeUint64ToSmi(input) ==> MLR: __ SmiTag(input)
  // ECL: ChangeIntPtrToSmi(input) ==> MLR: __ SmiTag(input)
  // ECL: ChangeFloat64ToTagged(i, m) ==> MLR: __ ConvertFloat64ToNumber(i, m)
  // ECL: ChangeSmiToIntPtr(input)
  //   ==> MLR: __ ChangeInt32ToIntPtr(__ SmiUntag(input))
  // ECL: ChangeSmiToInt32(input) ==> MLR: __ SmiUntag(input)
  // ECL: ChangeSmiToInt64(input) ==> MLR: __ ChangeInt32ToInt64(__
  // SmiUntag(input))
  // ECL: BuildCheckedHeapNumberOrOddballToFloat64 ==> MLR:
  // ConvertHeapObjectToFloat64OrDeopt

 private:
  // TODO(nicohartmann@): Might move some of those helpers into the assembler
  // interface.
  // Pass {bitfield} = {digit} = OpIndex::Invalid() to construct the canonical
  // 0n BigInt.
  V<Tagged> AllocateBigInt(V<Word32> bitfield, V<Word64> digit) {
    DCHECK(Is64());
    DCHECK_EQ(bitfield.valid(), digit.valid());
    static constexpr auto zero_bitfield =
        BigInt::SignBits::update(BigInt::LengthBits::encode(0), false);

    V<Tagged> map = __ HeapConstant(factory_->bigint_map());
    V<Tagged> bigint =
        __ Allocate(__ IntPtrConstant(BigInt::SizeFor(digit.valid() ? 1 : 0)),
                    AllocationType::kYoung);
    __ StoreField(bigint, AccessBuilder::ForMap(), map);
    __ StoreField(
        bigint, AccessBuilder::ForBigIntBitfield(),
        bitfield.valid() ? bitfield : __ Word32Constant(zero_bitfield));

    // BigInts have no padding on 64 bit architectures with pointer compression.
    if (BigInt::HasOptionalPadding()) {
      __ StoreField(bigint, AccessBuilder::ForBigIntOptionalPadding(),
                    __ IntPtrConstant(0));
    }
    if (digit.valid()) {
      __ StoreField(bigint, AccessBuilder::ForBigIntLeastSignificantDigit64(),
                    digit);
    }
    return bigint;
  }

  // TODO(nicohartmann@): Should also make this an operation and lower in
  // TagUntagLoweringReducer.
  V<Word32> IsSmi(V<Tagged> input) {
    return __ Word32Equal(
        __ Word32BitwiseAnd(V<Word32>::Cast(input),
                            static_cast<uint32_t>(kSmiTagMask)),
        static_cast<uint32_t>(kSmiTag));
  }

  void SmiTagOrOverflow(V<Word32> input, Label<>* overflow,
                        Label<Tagged>* done) {
    DCHECK(SmiValuesAre31Bits());

    // Check for overflow at the same time that we are smi tagging.
    // Since smi tagging shifts left by one, it's the same as adding value
    // twice.
    OpIndex add = __ Int32AddCheckOverflow(input, input);
    V<Word32> check = __ Projection(add, 1, WordRepresentation::Word32());
    GOTO_IF(check, *overflow);
    GOTO(*done, __ SmiTag(input));
  }

  V<Tagged> AllocateHeapNumberWithValue(V<Float64> value) {
    V<Tagged> result = __ Allocate(__ IntPtrConstant(HeapNumber::kSize),
                                   AllocationType::kYoung);
    __ StoreField(result, AccessBuilder::ForMap(),
                  __ HeapConstant(factory_->heap_number_map()));
    __ StoreField(result, AccessBuilder::ForHeapNumberValue(), value);
    return result;
  }

  V<Float64> ConvertHeapObjectToFloat64OrDeopt(
      V<Tagged> heap_object, OpIndex frame_state,
      ConvertObjectToPrimitiveOrDeoptOp::ObjectKind input_kind,
      const FeedbackSource& feedback) {
    V<Tagged> map = __ LoadMapField(heap_object);
    V<Word32> check_number =
        __ TaggedEqual(map, __ HeapConstant(factory_->heap_number_map()));
    switch (input_kind) {
      case ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kSmi:
      case ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kNumberOrString:
        UNREACHABLE();
      case ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kNumber: {
        __ DeoptimizeIfNot(check_number, frame_state,
                           DeoptimizeReason::kNotAHeapNumber, feedback);
        break;
      }
      case ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kNumberOrBoolean: {
        IF_NOT(check_number) {
          STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                            Oddball::kToNumberRawOffset);
          __ DeoptimizeIfNot(
              __ TaggedEqual(map, __ HeapConstant(factory_->boolean_map())),
              frame_state, DeoptimizeReason::kNotANumberOrBoolean, feedback);
        }
        END_IF
        break;
      }
      case ConvertObjectToPrimitiveOrDeoptOp::ObjectKind::kNumberOrOddball: {
        IF_NOT(check_number) {
          // For oddballs also contain the numeric value, let us just check that
          // we have an oddball here.
          STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                            Oddball::kToNumberRawOffset);
          V<Word32> instance_type = __ template LoadField<Word32>(
              map, AccessBuilder::ForMapInstanceType());
          __ DeoptimizeIfNot(__ Word32Equal(instance_type, ODDBALL_TYPE),
                             frame_state,
                             DeoptimizeReason::kNotANumberOrOddball, feedback);
        }
        END_IF
        break;
      }
    }
    return __ template LoadField<Float64>(heap_object,
                                          AccessBuilder::ForHeapNumberValue());
  }

  Factory* factory_;
};

#include "src/compiler/turboshaft/undef-assembler-macros.inc"

}  // namespace v8::internal::compiler::turboshaft

#endif  // V8_COMPILER_TURBOSHAFT_MACHINE_LOWERING_REDUCER_H_
