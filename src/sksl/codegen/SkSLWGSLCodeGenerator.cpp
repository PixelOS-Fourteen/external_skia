/*
 * Copyright 2022 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/sksl/codegen/SkSLWGSLCodeGenerator.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/core/SkSpan.h"
#include "include/core/SkTypes.h"
#include "include/private/SkBitmaskEnum.h"
#include "include/private/SkSLIRNode.h"
#include "include/private/SkSLLayout.h"
#include "include/private/SkSLModifiers.h"
#include "include/private/SkSLProgramElement.h"
#include "include/private/SkSLStatement.h"
#include "include/private/SkSLString.h"
#include "include/private/SkSLSymbol.h"
#include "include/sksl/SkSLErrorReporter.h"
#include "include/sksl/SkSLOperator.h"
#include "include/sksl/SkSLPosition.h"
#include "src/sksl/SkSLAnalysis.h"
#include "src/sksl/SkSLBuiltinTypes.h"
#include "src/sksl/SkSLCompiler.h"
#include "src/sksl/SkSLContext.h"
#include "src/sksl/SkSLOutputStream.h"
#include "src/sksl/SkSLProgramSettings.h"
#include "src/sksl/SkSLStringStream.h"
#include "src/sksl/SkSLUtil.h"
#include "src/sksl/analysis/SkSLProgramVisitor.h"
#include "src/sksl/ir/SkSLBinaryExpression.h"
#include "src/sksl/ir/SkSLBlock.h"
#include "src/sksl/ir/SkSLConstructor.h"
#include "src/sksl/ir/SkSLConstructorCompound.h"
#include "src/sksl/ir/SkSLExpression.h"
#include "src/sksl/ir/SkSLExpressionStatement.h"
#include "src/sksl/ir/SkSLFieldAccess.h"
#include "src/sksl/ir/SkSLFunctionCall.h"
#include "src/sksl/ir/SkSLFunctionDeclaration.h"
#include "src/sksl/ir/SkSLFunctionDefinition.h"
#include "src/sksl/ir/SkSLInterfaceBlock.h"
#include "src/sksl/ir/SkSLLiteral.h"
#include "src/sksl/ir/SkSLProgram.h"
#include "src/sksl/ir/SkSLReturnStatement.h"
#include "src/sksl/ir/SkSLStructDefinition.h"
#include "src/sksl/ir/SkSLSwizzle.h"
#include "src/sksl/ir/SkSLSymbolTable.h"
#include "src/sksl/ir/SkSLTernaryExpression.h"
#include "src/sksl/ir/SkSLType.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"
#include "src/sksl/ir/SkSLVariable.h"
#include "src/sksl/ir/SkSLVariableReference.h"

// TODO(skia:13092): This is a temporary debug feature. Remove when the implementation is
// complete and this is no longer needed.
#define DUMP_SRC_IR 0

namespace SkSL {

enum class ProgramKind : int8_t;

namespace {

// See https://www.w3.org/TR/WGSL/#memory-view-types
enum class PtrAddressSpace {
    kFunction,
    kPrivate,
    kStorage,
};

std::string_view pipeline_struct_prefix(ProgramKind kind) {
    if (ProgramConfig::IsVertex(kind)) {
        return "VS";
    }
    if (ProgramConfig::IsFragment(kind)) {
        return "FS";
    }
    return "";
}

std::string_view address_space_to_str(PtrAddressSpace addressSpace) {
    switch (addressSpace) {
        case PtrAddressSpace::kFunction:
            return "function";
        case PtrAddressSpace::kPrivate:
            return "private";
        case PtrAddressSpace::kStorage:
            return "storage";
    }
    SkDEBUGFAIL("unsupported ptr address space");
    return "unsupported";
}

std::string_view to_scalar_type(const Type& type) {
    SkASSERT(type.typeKind() == Type::TypeKind::kScalar);
    switch (type.numberKind()) {
        // Floating-point numbers in WebGPU currently always have 32-bit footprint and
        // relaxed-precision is not supported without extensions. f32 is the only floating-point
        // number type in WGSL (see the discussion on https://github.com/gpuweb/gpuweb/issues/658).
        case Type::NumberKind::kFloat:
            return "f32";
        case Type::NumberKind::kSigned:
            return "i32";
        case Type::NumberKind::kUnsigned:
            return "u32";
        case Type::NumberKind::kBoolean:
            return "bool";
        case Type::NumberKind::kNonnumeric:
            [[fallthrough]];
        default:
            break;
    }
    return type.name();
}

// Convert a SkSL type to a WGSL type. Handles all plain types except structure types
// (see https://www.w3.org/TR/WGSL/#plain-types-section).
std::string to_wgsl_type(const Type& type) {
    // TODO(skia:13092): Handle array, matrix, sampler types.
    switch (type.typeKind()) {
        case Type::TypeKind::kScalar:
            return std::string(to_scalar_type(type));
        case Type::TypeKind::kVector: {
            std::string_view ct = to_scalar_type(type.componentType());
            return String::printf("vec%d<%.*s>", type.columns(), (int)ct.length(), ct.data());
        }
        case Type::TypeKind::kArray: {
            std::string elementType = to_wgsl_type(type.componentType());
            if (type.isUnsizedArray()) {
                return String::printf("array<%s>", elementType.c_str());
            }
            return String::printf("array<%s, %d>", elementType.c_str(), type.columns());
        }
        default:
            break;
    }
    return std::string(type.name());
}

std::string to_ptr_type(const Type& type,
                        PtrAddressSpace addressSpace = PtrAddressSpace::kFunction) {
    return "ptr<" + std::string(address_space_to_str(addressSpace)) + ", " + to_wgsl_type(type) +
           ">";
}

std::string_view wgsl_builtin_name(WGSLCodeGenerator::Builtin builtin) {
    using Builtin = WGSLCodeGenerator::Builtin;
    switch (builtin) {
        case Builtin::kVertexIndex:
            return "vertex_index";
        case Builtin::kInstanceIndex:
            return "instance_index";
        case Builtin::kPosition:
            return "position";
        case Builtin::kFrontFacing:
            return "front_facing";
        case Builtin::kSampleIndex:
            return "sample_index";
        case Builtin::kFragDepth:
            return "frag_depth";
        case Builtin::kSampleMask:
            return "sample_mask";
        case Builtin::kLocalInvocationId:
            return "local_invocation_id";
        case Builtin::kLocalInvocationIndex:
            return "local_invocation_index";
        case Builtin::kGlobalInvocationId:
            return "global_invocation_id";
        case Builtin::kWorkgroupId:
            return "workgroup_id";
        case Builtin::kNumWorkgroups:
            return "num_workgroups";
        default:
            break;
    }

    SkDEBUGFAIL("unsupported builtin");
    return "unsupported";
}

std::string_view wgsl_builtin_type(WGSLCodeGenerator::Builtin builtin) {
    using Builtin = WGSLCodeGenerator::Builtin;
    switch (builtin) {
        case Builtin::kVertexIndex:
            return "u32";
        case Builtin::kInstanceIndex:
            return "u32";
        case Builtin::kPosition:
            return "vec4<f32>";
        case Builtin::kFrontFacing:
            return "bool";
        case Builtin::kSampleIndex:
            return "u32";
        case Builtin::kFragDepth:
            return "f32";
        case Builtin::kSampleMask:
            return "u32";
        case Builtin::kLocalInvocationId:
            return "vec3<u32>";
        case Builtin::kLocalInvocationIndex:
            return "u32";
        case Builtin::kGlobalInvocationId:
            return "vec3<u32>";
        case Builtin::kWorkgroupId:
            return "vec3<u32>";
        case Builtin::kNumWorkgroups:
            return "vec3<u32>";
        default:
            break;
    }

    SkDEBUGFAIL("unsupported builtin");
    return "unsupported";
}

// Some built-in variables have a type that differs from their SkSL counterpart (e.g. signed vs
// unsigned integer). We handle these cases with an explicit type conversion during a variable
// reference. Returns the WGSL type of the conversion target if conversion is needed, otherwise
// returns std::nullopt.
std::optional<std::string_view> needs_builtin_type_conversion(const Variable& v) {
    switch (v.modifiers().fLayout.fBuiltin) {
        case SK_VERTEXID_BUILTIN:
        case SK_INSTANCEID_BUILTIN:
            return {"i32"};
        default:
            break;
    }
    return std::nullopt;
}

// Map a SkSL builtin flag to a WGSL builtin kind. Returns std::nullopt if `builtin` is not
// not supported for WGSL.
//
// Also see //src/sksl/sksl_vert.sksl and //src/sksl/sksl_frag.sksl for supported built-ins.
std::optional<WGSLCodeGenerator::Builtin> builtin_from_sksl_name(int builtin) {
    using Builtin = WGSLCodeGenerator::Builtin;
    switch (builtin) {
        case SK_POSITION_BUILTIN:
            [[fallthrough]];
        case SK_FRAGCOORD_BUILTIN:
            return {Builtin::kPosition};
        case SK_VERTEXID_BUILTIN:
            return {Builtin::kVertexIndex};
        case SK_INSTANCEID_BUILTIN:
            return {Builtin::kInstanceIndex};
        case SK_CLOCKWISE_BUILTIN:
            // TODO(skia:13092): While `front_facing` is the corresponding built-in, it does not
            // imply a particular winding order. We correctly compute the face orientation based
            // on how Skia configured the render pipeline for all references to this built-in
            // variable (see `SkSL::Program::Inputs::fUseFlipRTUniform`).
            return {Builtin::kFrontFacing};
        default:
            break;
    }
    return std::nullopt;
}

const SymbolTable* top_level_symbol_table(const FunctionDefinition& f) {
    return f.body()->as<Block>().symbolTable()->fParent.get();
}

const char* delimiter_to_str(WGSLCodeGenerator::Delimiter delimiter) {
    using Delim = WGSLCodeGenerator::Delimiter;
    switch (delimiter) {
        case Delim::kComma:
            return ",";
        case Delim::kSemicolon:
            return ";";
        case Delim::kNone:
        default:
            break;
    }
    return "";
}

// FunctionDependencyResolver visits the IR tree rooted at a particular function definition and
// computes that function's dependencies on pipeline stage IO parameters. These are later used to
// synthesize arguments when writing out function definitions.
class FunctionDependencyResolver : public ProgramVisitor {
public:
    using Deps = WGSLCodeGenerator::FunctionDependencies;
    using DepsMap = WGSLCodeGenerator::ProgramRequirements::DepsMap;

    FunctionDependencyResolver(const Program* p,
                               const FunctionDeclaration* f,
                               DepsMap* programDependencyMap)
            : fProgram(p), fFunction(f), fDependencyMap(programDependencyMap) {}

    Deps resolve() {
        fDeps = Deps::kNone;
        this->visit(*fProgram);
        return fDeps;
    }

private:
    bool visitProgramElement(const ProgramElement& p) override {
        // Only visit the program that matches the requested function.
        if (p.is<FunctionDefinition>() && &p.as<FunctionDefinition>().declaration() == fFunction) {
            return INHERITED::visitProgramElement(p);
        }
        // Continue visiting other program elements.
        return false;
    }

    bool visitExpression(const Expression& e) override {
        if (e.is<VariableReference>()) {
            const VariableReference& v = e.as<VariableReference>();
            const Modifiers& modifiers = v.variable()->modifiers();
            if (v.variable()->storage() == Variable::Storage::kGlobal) {
                if (modifiers.fFlags & Modifiers::kIn_Flag) {
                    fDeps |= Deps::kPipelineInputs;
                }
                if (modifiers.fFlags & Modifiers::kOut_Flag) {
                    fDeps |= Deps::kPipelineOutputs;
                }
            }
        } else if (e.is<FunctionCall>()) {
            // The current function that we're processing (`fFunction`) inherits the dependencies of
            // functions that it makes calls to, because the pipeline stage IO parameters need to be
            // passed down as an argument.
            const FunctionCall& callee = e.as<FunctionCall>();

            // Don't process a function again if we have already resolved it.
            Deps* found = fDependencyMap->find(&callee.function());
            if (found) {
                fDeps |= *found;
            } else {
                // Store the dependencies that have been discovered for the current function so far.
                // If `callee` directly or indirectly calls the current function, then this value
                // will prevent an infinite recursion.
                fDependencyMap->set(fFunction, fDeps);

                // Separately traverse the called function's definition and determine its
                // dependencies.
                FunctionDependencyResolver resolver(fProgram, &callee.function(), fDependencyMap);
                Deps calleeDeps = resolver.resolve();

                // Store the callee's dependencies in the global map to avoid processing
                // the function again for future calls.
                fDependencyMap->set(&callee.function(), calleeDeps);

                // Add to the current function's dependencies.
                fDeps |= calleeDeps;
            }
        }
        return INHERITED::visitExpression(e);
    }

    const Program* const fProgram;
    const FunctionDeclaration* const fFunction;
    DepsMap* const fDependencyMap;
    Deps fDeps = Deps::kNone;

    using INHERITED = ProgramVisitor;
};

WGSLCodeGenerator::ProgramRequirements resolve_program_requirements(const Program* program) {
    bool mainNeedsCoordsArgument = false;
    WGSLCodeGenerator::ProgramRequirements::DepsMap dependencies;

    for (const ProgramElement* e : program->elements()) {
        if (!e->is<FunctionDefinition>()) {
            continue;
        }

        const FunctionDeclaration& decl = e->as<FunctionDefinition>().declaration();
        if (decl.isMain()) {
            for (const Variable* v : decl.parameters()) {
                if (v->modifiers().fLayout.fBuiltin == SK_MAIN_COORDS_BUILTIN) {
                    mainNeedsCoordsArgument = true;
                    break;
                }
            }
        }

        FunctionDependencyResolver resolver(program, &decl, &dependencies);
        dependencies.set(&decl, resolver.resolve());
    }

    return WGSLCodeGenerator::ProgramRequirements(std::move(dependencies), mainNeedsCoordsArgument);
}

int count_pipeline_inputs(const Program* program) {
    int inputCount = 0;
    for (const ProgramElement* e : program->elements()) {
        if (e->is<GlobalVarDeclaration>()) {
            const Variable* v = e->as<GlobalVarDeclaration>().varDeclaration().var();
            if (v->modifiers().fFlags & Modifiers::kIn_Flag) {
                inputCount++;
            }
        } else if (e->is<InterfaceBlock>()) {
            const Variable* v = e->as<InterfaceBlock>().var();
            if (v->modifiers().fFlags & Modifiers::kIn_Flag) {
                inputCount++;
            }
        }
    }
    return inputCount;
}

static bool is_in_global_uniforms(const Variable& var) {
    SkASSERT(var.storage() == VariableStorage::kGlobal);
    return var.modifiers().fFlags & Modifiers::kUniform_Flag && !var.type().isOpaque();
}

}  // namespace

bool WGSLCodeGenerator::generateCode() {
    // The resources of a WGSL program are structured in the following way:
    // - Vertex and fragment stage attribute inputs and outputs are bundled
    //   inside synthetic structs called VSIn/VSOut/FSIn/FSOut.
    // - All uniform and storage type resources are declared in global scope.
    this->preprocessProgram();

    StringStream header;
    {
        AutoOutputStream outputToHeader(this, &header, &fIndentation);
        // TODO(skia:13092): Implement the following:
        // - global uniform/storage resource declarations, including interface blocks.
        this->writeStageInputStruct();
        this->writeStageOutputStruct();
        this->writeNonBlockUniformsForTests();
    }
    StringStream body;
    {
        AutoOutputStream outputToBody(this, &body, &fIndentation);
        for (const ProgramElement* e : fProgram.elements()) {
            this->writeProgramElement(*e);
        }

// TODO(skia:13092): This is a temporary debug feature. Remove when the implementation is
// complete and this is no longer needed.
#if DUMP_SRC_IR
        this->writeLine("\n----------");
        this->writeLine("Source IR:\n");
        for (const ProgramElement* e : fProgram.elements()) {
            this->writeLine(e->description().c_str());
        }
#endif
    }

    write_stringstream(header, *fOut);
    write_stringstream(body, *fOut);
    return fContext.fErrors->errorCount() == 0;
}

void WGSLCodeGenerator::preprocessProgram() {
    fRequirements = resolve_program_requirements(&fProgram);
    fPipelineInputCount = count_pipeline_inputs(&fProgram);
}

void WGSLCodeGenerator::write(std::string_view s) {
    if (s.empty()) {
        return;
    }
    if (fAtLineStart) {
        for (int i = 0; i < fIndentation; i++) {
            fOut->writeText("    ");
        }
    }
    fOut->writeText(std::string(s).c_str());
    fAtLineStart = false;
}

void WGSLCodeGenerator::writeLine(std::string_view s) {
    this->write(s);
    fOut->writeText("\n");
    fAtLineStart = true;
}

void WGSLCodeGenerator::finishLine() {
    if (!fAtLineStart) {
        this->writeLine();
    }
}

void WGSLCodeGenerator::writeName(std::string_view name) {
    // Add underscore before name to avoid conflict with reserved words.
    if (fReservedWords.contains(name)) {
        this->write("_");
    }
    this->write(name);
}

void WGSLCodeGenerator::writeVariableDecl(const Type& type,
                                          std::string_view name,
                                          Delimiter delimiter) {
    this->writeName(name);
    this->write(": " + to_wgsl_type(type));
    this->writeLine(delimiter_to_str(delimiter));
}

void WGSLCodeGenerator::writePipelineIODeclaration(Modifiers modifiers,
                                                   const Type& type,
                                                   std::string_view name,
                                                   Delimiter delimiter) {
    // In WGSL, an entry-point IO parameter is "one of either a built-in value or
    // assigned a location". However, some SkSL declarations, specifically sk_FragColor, can
    // contain both a location and a builtin modifier. In addition, WGSL doesn't have a built-in
    // equivalent for sk_FragColor as it relies on the user-defined location for a render
    // target.
    //
    // Instead of special-casing sk_FragColor, we just give higher precedence to a location
    // modifier if a declaration happens to both have a location and it's a built-in.
    //
    // Also see:
    // https://www.w3.org/TR/WGSL/#input-output-locations
    // https://www.w3.org/TR/WGSL/#attribute-location
    // https://www.w3.org/TR/WGSL/#builtin-inputs-outputs
    int location = modifiers.fLayout.fLocation;
    if (location >= 0) {
        this->writeUserDefinedIODecl(type, name, location, delimiter);
    } else if (modifiers.fLayout.fBuiltin >= 0) {
        auto builtin = builtin_from_sksl_name(modifiers.fLayout.fBuiltin);
        if (builtin.has_value()) {
            this->writeBuiltinIODecl(type, name, *builtin, delimiter);
        }
    }
}

void WGSLCodeGenerator::writeUserDefinedIODecl(const Type& type,
                                               std::string_view name,
                                               int location,
                                               Delimiter delimiter) {
    this->write("@location(" + std::to_string(location) + ") ");

    // "User-defined IO of scalar or vector integer type must always be specified as
    // @interpolate(flat)" (see https://www.w3.org/TR/WGSL/#interpolation)
    if (type.isInteger() || (type.isVector() && type.componentType().isInteger())) {
        this->write("@interpolate(flat) ");
    }

    this->writeVariableDecl(type, name, delimiter);
}

void WGSLCodeGenerator::writeBuiltinIODecl(const Type& type,
                                           std::string_view name,
                                           Builtin builtin,
                                           Delimiter delimiter) {
    this->write("@builtin(");
    this->write(wgsl_builtin_name(builtin));
    this->write(") ");

    this->writeName(name);
    this->write(": ");
    this->write(wgsl_builtin_type(builtin));
    this->writeLine(delimiter_to_str(delimiter));
}

void WGSLCodeGenerator::writeFunction(const FunctionDefinition& f) {
    this->writeFunctionDeclaration(f.declaration());
    this->write(" ");
    this->writeBlock(f.body()->as<Block>());

    if (f.declaration().isMain()) {
        // We just emitted the user-defined main function. Next, we generate a program entry point
        // that calls the user-defined main.
        this->writeEntryPoint(f);
    }
}

void WGSLCodeGenerator::writeFunctionDeclaration(const FunctionDeclaration& f) {
    this->write("fn ");
    this->write(f.mangledName());
    this->write("(");
    auto separator = SkSL::String::Separator();
    FunctionDependencies* deps = fRequirements.dependencies.find(&f);
    if (deps) {
        std::string_view structNamePrefix = pipeline_struct_prefix(fProgram.fConfig->fKind);
        if (structNamePrefix.length() != 0) {
            if ((*deps & FunctionDependencies::kPipelineInputs) != FunctionDependencies::kNone) {
                this->write(separator());
                this->write("_stageIn: ");
                this->write(structNamePrefix);
                this->write("In");
            }
            if ((*deps & FunctionDependencies::kPipelineOutputs) != FunctionDependencies::kNone) {
                this->write(separator());
                this->write("_stageOut: ptr<function, ");
                this->write(structNamePrefix);
                this->write("Out>");
            }
        }
    }
    for (const Variable* param : f.parameters()) {
        this->write(separator());
        this->writeName(param->mangledName());
        this->write(": ");

        // Declare an "out" function parameter as a pointer.
        if (param->modifiers().fFlags & Modifiers::kOut_Flag) {
            this->write(to_ptr_type(param->type()));
        } else {
            this->write(to_wgsl_type(param->type()));
        }
    }
    this->write(")");
    if (!f.returnType().isVoid()) {
        this->write(" -> ");
        this->write(to_wgsl_type(f.returnType()));
    }
}

void WGSLCodeGenerator::writeEntryPoint(const FunctionDefinition& main) {
    SkASSERT(main.declaration().isMain());

    // The input and output parameters for a vertex/fragment stage entry point function have the
    // FSIn/FSOut/VSIn/VSOut struct types that have been synthesized in generateCode(). An entry
    // point always has the same signature and acts as a trampoline to the user-defined main
    // function.
    std::string outputType;
    if (ProgramConfig::IsVertex(fProgram.fConfig->fKind)) {
        this->write("@vertex fn vertexMain(");
        if (fPipelineInputCount > 0) {
            this->write("_stageIn: VSIn");
        }
        this->writeLine(") -> VSOut {");
        outputType = "VSOut";
    } else if (ProgramConfig::IsFragment(fProgram.fConfig->fKind)) {
        this->write("@fragment fn fragmentMain(");
        if (fPipelineInputCount > 0) {
            this->write("_stageIn: FSIn");
        }
        this->writeLine(") -> FSOut {");
        outputType = "FSOut";
    } else {
        fContext.fErrors->error(Position(), "program kind not supported");
        return;
    }

    // Declare the stage output struct.
    fIndentation++;
    this->write("var _stageOut: ");
    this->write(outputType);
    this->writeLine(";");

    // Generate assignment to sk_FragColor built-in if the user-defined main returns a color.
    if (ProgramConfig::IsFragment(fProgram.fConfig->fKind)) {
        const SymbolTable* symbolTable = top_level_symbol_table(main);
        const Symbol* symbol = symbolTable->find("sk_FragColor");
        SkASSERT(symbol);
        if (main.declaration().returnType().matches(symbol->type())) {
            this->write("_stageOut.sk_FragColor = ");
        }
    }

    // Generate the function call to the user-defined main:
    this->write(main.declaration().mangledName());
    this->write("(");
    auto separator = SkSL::String::Separator();
    FunctionDependencies* deps = fRequirements.dependencies.find(&main.declaration());
    if (deps) {
        if ((*deps & FunctionDependencies::kPipelineInputs) != FunctionDependencies::kNone) {
            this->write(separator());
            this->write("_stageIn");
        }
        if ((*deps & FunctionDependencies::kPipelineOutputs) != FunctionDependencies::kNone) {
            this->write(separator());
            this->write("&_stageOut");
        }
    }
    // TODO(armansito): Handle arbitrary parameters.
    if (main.declaration().parameters().size() != 0) {
        const Variable* v = main.declaration().parameters()[0];
        const Type& type = v->type();
        if (v->modifiers().fLayout.fBuiltin == SK_MAIN_COORDS_BUILTIN) {
            if (!type.matches(*fContext.fTypes.fFloat2)) {
                fContext.fErrors->error(
                        main.fPosition,
                        "main function has unsupported parameter: " + type.description());
                return;
            }

            this->write(separator());
            this->write("_stageIn.sk_FragCoord.xy");
        }
    }
    this->writeLine(");");
    this->writeLine("return _stageOut;");

    fIndentation--;
    this->writeLine("}");
}

void WGSLCodeGenerator::writeStatement(const Statement& s) {
    switch (s.kind()) {
        case Statement::Kind::kBlock:
            this->writeBlock(s.as<Block>());
            break;
        case Statement::Kind::kExpression:
            this->writeExpressionStatement(s.as<ExpressionStatement>());
            break;
        case Statement::Kind::kReturn:
            this->writeReturnStatement(s.as<ReturnStatement>());
            break;
        case Statement::Kind::kVarDeclaration:
            this->writeVarDeclaration(s.as<VarDeclaration>());
            break;
        default:
            SkDEBUGFAILF("unsupported statement (kind: %d) %s",
                         static_cast<int>(s.kind()), s.description().c_str());
            break;
    }
}

void WGSLCodeGenerator::writeStatements(const StatementArray& statements) {
    for (const auto& s : statements) {
        if (!s->isEmpty()) {
            this->writeStatement(*s);
            this->finishLine();
        }
    }
}

void WGSLCodeGenerator::writeBlock(const Block& b) {
    // Write scope markers if this block is a scope, or if the block is empty (since we need to emit
    // something here to make the code valid).
    bool isScope = b.isScope() || b.isEmpty();
    if (isScope) {
        this->writeLine("{");
        fIndentation++;
    }
    this->writeStatements(b.children());
    if (isScope) {
        fIndentation--;
        this->writeLine("}");
    }
}

void WGSLCodeGenerator::writeExpressionStatement(const ExpressionStatement& s) {
    if (Analysis::HasSideEffects(*s.expression())) {
        this->writeExpression(*s.expression(), Precedence::kTopLevel);
        this->write(";");
    }
}

void WGSLCodeGenerator::writeReturnStatement(const ReturnStatement& s) {
    this->write("return");
    if (s.expression()) {
        this->write(" ");
        this->writeExpression(*s.expression(), Precedence::kTopLevel);
    }
    this->write(";");
}

void WGSLCodeGenerator::writeVarDeclaration(const VarDeclaration& varDecl) {
    bool isConst = varDecl.var()->modifiers().fFlags & Modifiers::kConst_Flag;
    if (isConst) {
        this->write("let ");
    } else {
        this->write("var ");
    }
    this->writeName(varDecl.var()->mangledName());
    this->write(": ");
    this->write(to_wgsl_type(varDecl.var()->type()));

    if (varDecl.value()) {
        this->write(" = ");
        this->writeExpression(*varDecl.value(), Precedence::kTopLevel);
    } else if (isConst) {
        SkDEBUGFAILF("A let-declared constant must specify a value");
    }

    this->write(";");
}

void WGSLCodeGenerator::writeExpression(const Expression& e, Precedence parentPrecedence) {
    switch (e.kind()) {
        case Expression::Kind::kBinary:
            this->writeBinaryExpression(e.as<BinaryExpression>(), parentPrecedence);
            break;
        case Expression::Kind::kConstructorCompound:
            this->writeConstructorCompound(e.as<ConstructorCompound>(), parentPrecedence);
            break;
        case Expression::Kind::kConstructorCompoundCast:
        case Expression::Kind::kConstructorScalarCast:
            this->writeAnyConstructor(e.asAnyConstructor(), parentPrecedence);
            break;
        case Expression::Kind::kFieldAccess:
            this->writeFieldAccess(e.as<FieldAccess>());
            break;
        case Expression::Kind::kLiteral:
            this->writeLiteral(e.as<Literal>());
            break;
        case Expression::Kind::kSwizzle:
            this->writeSwizzle(e.as<Swizzle>());
            break;
        case Expression::Kind::kTernary:
            this->writeTernaryExpression(e.as<TernaryExpression>(), parentPrecedence);
            break;
        case Expression::Kind::kVariableReference:
            this->writeVariableReference(e.as<VariableReference>());
            break;
        default:
            SkDEBUGFAILF("unsupported expression (kind: %d) %s",
                         static_cast<int>(e.kind()),
                         e.description().c_str());
            break;
    }
}

void WGSLCodeGenerator::writeBinaryExpression(const BinaryExpression& b,
                                              Precedence parentPrecedence) {
    const Expression& left = *b.left();
    const Expression& right = *b.right();
    Operator op = b.getOperator();
    Precedence precedence = op.getBinaryPrecedence();
    bool needParens = precedence >= parentPrecedence;

    if (needParens) {
        this->write("(");
    }

    // TODO(skia:13092): Correctly handle the case when lhs is a pointer.

    this->writeExpression(left, precedence);
    this->write(op.operatorName());
    this->writeExpression(right, precedence);

    if (needParens) {
        this->write(")");
    }
}

void WGSLCodeGenerator::writeFieldAccess(const FieldAccess& f) {
    const Type::Field* field = &f.base()->type().fields()[f.fieldIndex()];
    if (FieldAccess::OwnerKind::kDefault == f.ownerKind()) {
        this->writeExpression(*f.base(), Precedence::kPostfix);
        this->write(".");
    } else {
        // We are accessing a field in an anonymous interface block. If the field refers to a
        // pipeline IO parameter, then we access it via the synthesized IO structs. We make an
        // explicit exception for `sk_PointSize` which we declare as a placeholder variable in
        // global scope as it is not supported by WebGPU as a pipeline IO parameter (see comments
        // in `writeStageOutputStruct`).
        const Variable& v = *f.base()->as<VariableReference>().variable();
        if (v.modifiers().fFlags & Modifiers::kIn_Flag) {
            this->write("_stageIn.");
        } else if (v.modifiers().fFlags & Modifiers::kOut_Flag &&
                   field->fModifiers.fLayout.fBuiltin != SK_POINTSIZE_BUILTIN) {
            this->write("(*_stageOut).");
        } else {
            // TODO(skia:13902): Reference the variable using the base name used for its
            // uniform/storage block global declaration.
        }
    }
    this->writeName(field->fName);
}

void WGSLCodeGenerator::writeLiteral(const Literal& l) {
    const Type& type = l.type();
    if (type.isFloat() || type.isBoolean()) {
        this->write(l.description(OperatorPrecedence::kTopLevel));
        return;
    }
    SkASSERT(type.isInteger());
    if (type.matches(*fContext.fTypes.fUInt)) {
        this->write(std::to_string(l.intValue() & 0xffffffff));
        this->write("u");
    } else if (type.matches(*fContext.fTypes.fUShort)) {
        this->write(std::to_string(l.intValue() & 0xffff));
        this->write("u");
    } else {
        this->write(std::to_string(l.intValue()));
    }
}

void WGSLCodeGenerator::writeSwizzle(const Swizzle& swizzle) {
    this->writeExpression(*swizzle.base(), Precedence::kPostfix);
    this->write(".");
    for (int c : swizzle.components()) {
        SkASSERT(c >= 0 && c <= 3);
        this->write(&("x\0y\0z\0w\0"[c * 2]));
    }
}

void WGSLCodeGenerator::writeTernaryExpression(const TernaryExpression& t,
                                               Precedence parentPrecedence) {
    bool needParens = Precedence::kTernary >= parentPrecedence;
    if (needParens) {
        this->write("(");
    }

    // The trivial case is when neither branch has side effects and evaluate to a scalar or vector
    // type. This can be represented with a call to the WGSL `select` intrinsic although it doesn't
    // support short-circuiting.
    if ((t.type().isScalar() || t.type().isVector()) && !Analysis::HasSideEffects(*t.ifTrue()) &&
        !Analysis::HasSideEffects(*t.ifFalse())) {
        this->write("select(");
        this->writeExpression(*t.ifFalse(), Precedence::kTernary);
        this->write(", ");
        this->writeExpression(*t.ifTrue(), Precedence::kTernary);
        this->write(", ");

        bool isVector = t.type().isVector();
        if (isVector) {
            // Splat the condition expression into a vector.
            this->write(String::printf("vec%d<bool>", t.type().columns()));
            this->write("(");
        }
        this->writeExpression(*t.test(), Precedence::kTernary);
        if (isVector) {
            this->write(")");
        }
        this->write(")");

        if (needParens) {
            this->write(")");
        }
        return;
    }

    // TODO(skia:13092): WGSL does not support ternary expressions. To replicate the required
    // short-circuting behavior we need to hoist the expression out into the surrounding block,
    // convert it into an if statement that writes the result to a synthesized variable, and replace
    // the original expression with a reference to that variable.
    //
    // Once hoisting is supported, we may want to use that for vector type expressions as well,
    // since select above does a component-wise select
}

void WGSLCodeGenerator::writeVariableReference(const VariableReference& r) {
    // TODO(skia:13902): Correctly handle function parameters.
    // TODO(skia:13902): Correctly handle RTflip for built-ins.
    const Variable& v = *r.variable();

    // Insert a conversion expression if this is a built-in variable whose type differs from the
    // SkSL.
    std::optional<std::string_view> conversion = needs_builtin_type_conversion(v);
    if (conversion.has_value()) {
        this->write(*conversion);
        this->write("(");
    }
    if (v.storage() == Variable::Storage::kGlobal) {
        if (v.modifiers().fFlags & Modifiers::kIn_Flag) {
            this->write("_stageIn.");
        } else if (v.modifiers().fFlags & Modifiers::kOut_Flag) {
            this->write("(*_stageOut).");
        } else if (is_in_global_uniforms(v)) {
            this->write("_globalUniforms.");
        }
    }

    this->writeName(v.mangledName());

    if (conversion.has_value()) {
        this->write(")");
    }
}

void WGSLCodeGenerator::writeAnyConstructor(const AnyConstructor& c, Precedence parentPrecedence) {
    this->write(to_wgsl_type(c.type()));
    this->write("(");
    auto separator = SkSL::String::Separator();
    for (const auto& e : c.argumentSpan()) {
        this->write(separator());
        this->writeExpression(*e, Precedence::kSequence);
    }
    this->write(")");
}

void WGSLCodeGenerator::writeConstructorCompound(const ConstructorCompound& c,
                                                 Precedence parentPrecedence) {
    // TODO(skia:13092): Support matrix constructors
    if (c.type().isVector()) {
        this->writeConstructorCompoundVector(c, parentPrecedence);
    } else {
        fContext.fErrors->error(c.fPosition, "unsupported compound constructor");
    }
}

void WGSLCodeGenerator::writeConstructorCompoundVector(const ConstructorCompound& c,
                                                       Precedence parentPrecedence) {
    // TODO(skia:13092): WGSL supports constructing vectors from a mix of scalars and vectors but
    // not matrices. SkSL supports vec4(mat2x2) which we need to handle here
    // (see https://www.w3.org/TR/WGSL/#type-constructor-expr).
    this->writeAnyConstructor(c, parentPrecedence);
}

void WGSLCodeGenerator::writeProgramElement(const ProgramElement& e) {
    switch (e.kind()) {
        case ProgramElement::Kind::kExtension:
            // TODO(skia:13092): WGSL supports extensions via the "enable" directive
            // (https://www.w3.org/TR/WGSL/#language-extensions). While we could easily emit this
            // directive, we should first ensure that all possible SkSL extension names are
            // converted to their appropriate WGSL extension. Currently there are no known supported
            // WGSL extensions aside from the hypotheticals listed in the spec.
            break;
        case ProgramElement::Kind::kGlobalVar:
            this->writeGlobalVarDeclaration(e.as<GlobalVarDeclaration>());
            break;
        case ProgramElement::Kind::kInterfaceBlock:
            // All interface block declarations are handled explicitly as the "program header" in
            // generateCode().
            break;
        case ProgramElement::Kind::kStructDefinition:
            this->writeStructDefinition(e.as<StructDefinition>());
            break;
        case ProgramElement::Kind::kFunctionPrototype:
            // A WGSL function declaration must contain its body and the function name is in scope
            // for the entire program (see https://www.w3.org/TR/WGSL/#function-declaration and
            // https://www.w3.org/TR/WGSL/#declaration-and-scope).
            //
            // As such, we don't emit function prototypes.
            break;
        case ProgramElement::Kind::kFunction:
            this->writeFunction(e.as<FunctionDefinition>());
            break;
        default:
            SkDEBUGFAILF("unsupported program element: %s\n", e.description().c_str());
            break;
    }
}

void WGSLCodeGenerator::writeGlobalVarDeclaration(const GlobalVarDeclaration& d) {
    const Variable& var = *d.declaration()->as<VarDeclaration>().var();
    if ((var.modifiers().fFlags & (Modifiers::kIn_Flag | Modifiers::kOut_Flag)) ||
        is_in_global_uniforms(var)) {
        // Pipeline stage I/O parameters and top-level (non-block) uniforms are handled specially
        // in generateCode().
        return;
    }

    // TODO(skia:13092): Implement workgroup variable decoration
    this->write("var<private> ");
    this->writeVariableDecl(var.type(), var.name(), Delimiter::kSemicolon);
}

void WGSLCodeGenerator::writeStructDefinition(const StructDefinition& s) {
    const Type& type = s.type();
    this->writeLine("struct " + type.displayName() + " {");
    fIndentation++;
    this->writeFields(SkSpan(type.fields()), type.fPosition);
    fIndentation--;
    this->writeLine("};");
}

void WGSLCodeGenerator::writeFields(SkSpan<const Type::Field> fields,
                                    Position parentPos,
                                    const MemoryLayout*) {
    // TODO(skia:13092): Check alignment against `layout` constraints, if present. A layout
    // constraint will be specified for interface blocks and for structs that appear in a block.
    for (const Type::Field& field : fields) {
        const Type* fieldType = field.fType;
        this->writeVariableDecl(*fieldType, field.fName, Delimiter::kComma);
    }
}

void WGSLCodeGenerator::writeStageInputStruct() {
    std::string_view structNamePrefix = pipeline_struct_prefix(fProgram.fConfig->fKind);
    if (structNamePrefix.empty()) {
        // There's no need to declare pipeline stage outputs.
        return;
    }

    // It is illegal to declare a struct with no members.
    if (fPipelineInputCount < 1) {
        return;
    }

    this->write("struct ");
    this->write(structNamePrefix);
    this->writeLine("In {");
    fIndentation++;

    bool declaredFragCoordsBuiltin = false;
    for (const ProgramElement* e : fProgram.elements()) {
        if (e->is<GlobalVarDeclaration>()) {
            const Variable* v = e->as<GlobalVarDeclaration>().declaration()
                                 ->as<VarDeclaration>().var();
            if (v->modifiers().fFlags & Modifiers::kIn_Flag) {
                this->writePipelineIODeclaration(v->modifiers(), v->type(), v->mangledName(),
                                                 Delimiter::kComma);
                if (v->modifiers().fLayout.fBuiltin == SK_FRAGCOORD_BUILTIN) {
                    declaredFragCoordsBuiltin = true;
                }
            }
        } else if (e->is<InterfaceBlock>()) {
            const Variable* v = e->as<InterfaceBlock>().var();
            // Merge all the members of `in` interface blocks to the input struct, which are
            // specified as either "builtin" or with a "layout(location=".
            //
            // TODO(armansito): Is it legal to have an interface block without a storage qualifier
            // but with members that have individual storage qualifiers?
            if (v->modifiers().fFlags & Modifiers::kIn_Flag) {
                for (const auto& f : v->type().fields()) {
                    this->writePipelineIODeclaration(f.fModifiers, *f.fType, f.fName,
                                                     Delimiter::kComma);
                    if (f.fModifiers.fLayout.fBuiltin == SK_FRAGCOORD_BUILTIN) {
                        declaredFragCoordsBuiltin = true;
                    }
                }
            }
        }
    }

    if (ProgramConfig::IsFragment(fProgram.fConfig->fKind) &&
        fRequirements.mainNeedsCoordsArgument && !declaredFragCoordsBuiltin) {
        this->writeLine("@builtin(position) sk_FragCoord: vec4<f32>,");
    }

    fIndentation--;
    this->writeLine("};");
}

void WGSLCodeGenerator::writeStageOutputStruct() {
    std::string_view structNamePrefix = pipeline_struct_prefix(fProgram.fConfig->fKind);
    if (structNamePrefix.empty()) {
        // There's no need to declare pipeline stage outputs.
        return;
    }

    this->write("struct ");
    this->write(structNamePrefix);
    this->writeLine("Out {");
    fIndentation++;

    // TODO(skia:13092): Remember all variables that are added to the output struct here so they
    // can be referenced correctly when handling variable references.
    bool declaredPositionBuiltin = false;
    bool requiresPointSizeBuiltin = false;
    for (const ProgramElement* e : fProgram.elements()) {
        if (e->is<GlobalVarDeclaration>()) {
            const Variable* v = e->as<GlobalVarDeclaration>().declaration()
                                 ->as<VarDeclaration>().var();
            if (v->modifiers().fFlags & Modifiers::kOut_Flag) {
                this->writePipelineIODeclaration(v->modifiers(), v->type(), v->mangledName(),
                                                 Delimiter::kComma);
            }
        } else if (e->is<InterfaceBlock>()) {
            const Variable* v = e->as<InterfaceBlock>().var();
            // Merge all the members of `out` interface blocks to the output struct, which are
            // specified as either "builtin" or with a "layout(location=".
            //
            // TODO(armansito): Is it legal to have an interface block without a storage qualifier
            // but with members that have individual storage qualifiers?
            if (v->modifiers().fFlags & Modifiers::kOut_Flag) {
                for (const auto& f : v->type().fields()) {
                    this->writePipelineIODeclaration(f.fModifiers, *f.fType, f.fName,
                                                     Delimiter::kComma);
                    if (f.fModifiers.fLayout.fBuiltin == SK_POSITION_BUILTIN) {
                        declaredPositionBuiltin = true;
                    } else if (f.fModifiers.fLayout.fBuiltin == SK_POINTSIZE_BUILTIN) {
                        // sk_PointSize is explicitly not supported by `builtin_from_sksl_name` so
                        // writePipelineIODeclaration will never write it. We mark it here if the
                        // declaration is needed so we can synthesize it below.
                        requiresPointSizeBuiltin = true;
                    }
                }
            }
        }
    }

    // A vertex program must include the `position` builtin in its entry point return type.
    if (ProgramConfig::IsVertex(fProgram.fConfig->fKind) && !declaredPositionBuiltin) {
        this->writeLine("@builtin(position) sk_Position: vec4<f32>,");
    }

    fIndentation--;
    this->writeLine("};");

    // In WebGPU/WGSL, the vertex stage does not support a point-size output and the size
    // of a point primitive is always 1 pixel (see https://github.com/gpuweb/gpuweb/issues/332).
    //
    // There isn't anything we can do to emulate this correctly at this stage so we
    // synthesize a placeholder variable that has no effect. Programs should not rely on
    // sk_PointSize when using the Dawn backend.
    if (ProgramConfig::IsVertex(fProgram.fConfig->fKind) && requiresPointSizeBuiltin) {
        this->writeLine("/* unsupported */ var<private> sk_PointSize: f32;");
    }
}

void WGSLCodeGenerator::writeNonBlockUniformsForTests() {
    for (const ProgramElement* e : fProgram.elements()) {
        if (e->is<GlobalVarDeclaration>()) {
            const GlobalVarDeclaration& decls = e->as<GlobalVarDeclaration>();
            const Variable& var = *decls.varDeclaration().var();
            if (is_in_global_uniforms(var)) {
                if (!fDeclaredUniformsStruct) {
                    this->write("struct _GlobalUniforms {\n");
                    fDeclaredUniformsStruct = true;
                }
                this->write("    ");
                this->writeVariableDecl(var.type(), var.mangledName(), Delimiter::kComma);
            }
        }
    }
    if (fDeclaredUniformsStruct) {
        int binding = fProgram.fConfig->fSettings.fDefaultUniformBinding;
        int set = fProgram.fConfig->fSettings.fDefaultUniformSet;
        this->write("};\n");
        this->write("@binding(" + std::to_string(binding) + ") ");
        this->write("@group(" + std::to_string(set) + ") ");
        this->writeLine("var<uniform> _globalUniforms: _GlobalUniforms;");
    }
}

}  // namespace SkSL
