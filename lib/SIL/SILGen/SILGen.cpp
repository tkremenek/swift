//===--- SILGen.cpp - Implements Lowering of ASTs -> SIL ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "llvm/ADT/Optional.h"
#include "swift/AST/AST.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/NameLookup.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/Mangle.h"
#include "swift/Subsystems.h"
#include "llvm/Support/Debug.h"
using namespace swift;
using namespace Lowering;

//===--------------------------------------------------------------------===//
// SILGenFunction Class implementation
//===--------------------------------------------------------------------===//

SILGenFunction::SILGenFunction(SILGenModule &SGM, SILFunction &F,
                               bool hasVoidReturn)
  : SGM(SGM), F(F), B(new (F.getModule()) SILBasicBlock(&F)),
    Cleanups(*this),
    hasVoidReturn(hasVoidReturn),
    epilogBB(nullptr) {
}

// FIXME: We should be able to simplify this after the FuncExpr and friends
// hierarhy is refactored.
static SILLocation getFuncBodySILLocation(SILLocation Func) {
  if (const FuncExpr *FE = Func.getAs<FuncExpr>())
    return SILLocation(FE->getBody());
  if (const PipeClosureExpr *FE = Func.getAs<PipeClosureExpr>())
    return SILLocation(FE->getBody());
  if (const ClosureExpr *FE = Func.getAs<ClosureExpr>())
    return SILLocation(FE->getBody());
  if (const ConstructorDecl *CD = Func.getAs<ConstructorDecl>())
    return SILLocation(CD->getBody());
  if (const DestructorDecl *DD = Func.getAs<DestructorDecl>())
    return SILLocation(DD->getBody());
  // FIXME: Should turn into assert after properties are handled.
  return SILLocation();
}

/// SILGenFunction destructor - called after the entire function's AST has been
/// visited.  This handles "falling off the end of the function" logic.
SILGenFunction::~SILGenFunction() {
  // If the end of the function isn't reachable (e.g. it ended in an explicit
  // return), then we're done.
  if (!B.hasValidInsertionPoint())
    return;
  
  // If we have an unterminated block, it is either an implicit return of an
  // empty tuple, or a dynamically unreachable location.
  if (hasVoidReturn) {
    assert(!epilogBB && "epilog bb not terminated?!");
    SILValue emptyTuple = emitEmptyTuple(SILLocation());
    Cleanups.emitReturnAndCleanups(SILLocation(), emptyTuple);
  } else {
    // FIXME: Get this from the SILFunction when SILFunction has SILLocation
    // info.
    B.createUnreachable(getFuncBodySILLocation(F.getLocation()));
  }
}

//===--------------------------------------------------------------------===//
// SILGenModule Class implementation
//===--------------------------------------------------------------------===//

SILGenModule::SILGenModule(SILModule &M)
  : M(M), Types(M.Types), TopLevelSGF(nullptr) {
  
  SILFunction *toplevel = emitTopLevelFunction();
  // Assign a debug scope pointing into the void to the top level function.
  toplevel->setDebugScope(new (M) SILDebugScope());

  TopLevelSGF = new SILGenFunction(*this, *toplevel,
                                   /*hasVoidReturn=*/true);
}

SILGenModule::~SILGenModule() {
  SILFunction *toplevel = &TopLevelSGF->getFunction();
  delete TopLevelSGF;
  DEBUG(llvm::dbgs() << "lowered toplevel sil:\n";
        toplevel->print(llvm::dbgs()));
  toplevel->verify();
  M.verify();
}

static SILDeclRef getBridgingFn(Optional<SILDeclRef> &cacheSlot,
                                 SILGenModule &SGM,
                                 StringRef moduleName,
                                 StringRef functionName,
                                 std::initializer_list<SILType> inputTypes,
                                 SILType outputType) {
  SILDeclRef fn = cacheSlot.cache([&] {
    Optional<UnqualifiedLookup> lookup
      = UnqualifiedLookup::forModuleAndName(SGM.M.getASTContext(),
                                            moduleName,
                                            functionName);
    // Check that we can find the module and function.
    // FIXME: Can we recover more gracefully?
    if (!lookup) {
      SGM.diagnose(SourceLoc(), diag::bridging_module_missing,
                   moduleName, functionName);
      exit(1);
    }
    if (lookup->Results.size() == 0) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_missing,
                   moduleName, functionName);
      exit(1);
    }
    // FIXME: Resolve overloads.
    if (lookup->Results.size() > 1) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_overloaded,
                   moduleName, functionName);
      exit(1);
    }
    auto &result = lookup->Results[0];
    // Check that the bridging function is actually a function.
    if (!result.hasValueDecl()) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_not_function,
                   moduleName, functionName);
      exit(1);
    }
    FuncDecl *fd = dyn_cast<FuncDecl>(result.getValueDecl());
    if (!fd) {
      SGM.diagnose(result.getValueDecl()->getLoc(),
                   diag::bridging_function_not_function,
                   moduleName, functionName);
      exit(1);
    }
    // Check that the function takes the expected arguments and returns the
    // expected result type.
    SILDeclRef c(fd);
    SILFunctionTypeInfo *funcInfo
      = SGM.getConstantType(c).getFunctionTypeInfo(SGM.M);
    
    if (funcInfo->getInputTypes().size() != inputTypes.size()
        || !std::equal(funcInfo->getInputTypes().begin(),
                       funcInfo->getInputTypes().end(),
                       inputTypes.begin())) {
      SGM.diagnose(fd->getLoc(), diag::bridging_function_not_correct_type,
                   moduleName, functionName);
      exit(1);
    }
    
    if (funcInfo->getResultType() != outputType) {
      SGM.diagnose(fd->getLoc(), diag::bridging_function_not_correct_type,
                   moduleName, functionName);
      exit(1);
    }
    
    return c;
  });
  
  DEBUG(llvm::dbgs() << "bridging function "
          << moduleName << '.' << functionName
          << " mapped to ";
        fn.print(llvm::dbgs()));
  
  return fn;
}

static SILType getByrefStringTy(SILGenModule &SGM) {
  return SGM.getLoweredType(LValueType::get(SGM.Types.getStringType(),
                                            LValueType::Qual::DefaultForType,
                                            SGM.M.getASTContext()));
}

static SILType getNSStringTy(SILGenModule &SGM) {
  return SGM.getLoweredType(SGM.Types.getNSStringType());
}

static SILType getBoolTy(SILGenModule &SGM) {
  return SGM.getLoweredType(SGM.Types.getBoolType());
}

static SILType getObjCBoolTy(SILGenModule &SGM) {
  return SGM.getLoweredType(SGM.Types.getObjCBoolType());
}

SILDeclRef SILGenModule::getNSStringToStringFn() {
  return getBridgingFn(NSStringToStringFn, *this,
                       "Foundation", "convertNSStringToString",
                       {getNSStringTy(*this), getByrefStringTy(*this)},
                       Types.getEmptyTupleType());
}

SILDeclRef SILGenModule::getStringToNSStringFn() {
  return getBridgingFn(StringToNSStringFn, *this,
                       "Foundation", "convertStringToNSString",
                       {getByrefStringTy(*this)},
                       getNSStringTy(*this));
}

SILDeclRef SILGenModule::getBoolToObjCBoolFn() {
  return getBridgingFn(BoolToObjCBoolFn, *this,
                       "ObjectiveC", "convertBoolToObjCBool",
                       {getBoolTy(*this)},
                       getObjCBoolTy(*this));
}

SILDeclRef SILGenModule::getObjCBoolToBoolFn() {
  return getBridgingFn(ObjCBoolToBoolFn, *this,
                       "ObjectiveC", "convertObjCBoolToBool",
                       {getObjCBoolTy(*this)},
                       getBoolTy(*this));
}

SILFunction *SILGenModule::emitTopLevelFunction() {
  ASTContext &C = M.getASTContext();
  Type topLevelType = FunctionType::get(TupleType::getEmpty(C),
                                        TupleType::getEmpty(C), C);
  SILType loweredType = getLoweredType(topLevelType);
  return new (M) SILFunction(M, SILLinkage::Internal,
                             "top_level_code", loweredType);
}

SILType SILGenModule::getConstantType(SILDeclRef constant) {
  return Types.getConstantType(constant);
}

SILLinkage SILGenModule::getConstantLinkage(SILDeclRef constant) {
  /// Anonymous functions always have internal linkage.
  if (!constant.hasDecl())
    return SILLinkage::Internal;
    
  ValueDecl *d = constant.getDecl();
  DeclContext *dc = d->getDeclContext();
  while (!dc->isModuleContext()) {
    if (dc->isLocalContext())
      return SILLinkage::Internal;
    dc = dc->getParent();
  }
  
  if (isa<ClangModule>(dc) &&
      (isa<ConstructorDecl>(d) ||
       isa<SubscriptDecl>(d) ||
       (isa<VarDecl>(d) && cast<VarDecl>(d)->isProperty())))
    return SILLinkage::ClangThunk;
  
  return SILLinkage::External;
}

SILFunction *SILGenModule::getFunction(SILDeclRef constant) {
  auto found = emittedFunctions.find(constant);
  if (found != emittedFunctions.end())
    return found->second;
  
  SILType constantType = getConstantType(constant);
  SILLinkage linkage = getConstantLinkage(constant);
  
  SILFunction *F = new (M) SILFunction(M, linkage, "", constantType);
  mangleConstant(constant, F);
  emittedFunctions[constant] = F;

  return F;
}

bool SILGenModule::hasFunction(SILDeclRef constant) {
  return emittedFunctions.count(constant);
}

void SILGenModule::visitFuncDecl(FuncDecl *fd) {
  emitFunction(fd, fd->getBody());
}

template<typename T>
SILFunction *SILGenModule::preEmitFunction(SILDeclRef constant, T *astNode) {
  SILFunction *f = getFunction(constant);
  assert(f->empty() && "already emitted function?!");

  // Create a debug scope for the function using astNode as source location.
  f->setDebugScope(new (M) SILDebugScope(astNode));

  f->setLocation(astNode);

  DEBUG(llvm::dbgs() << "lowering ";
        f->printName(llvm::dbgs());
        llvm::dbgs() << " : $";
        f->getLoweredType().print(llvm::dbgs());
        llvm::dbgs() << '\n';
        if (astNode) {
          astNode->print(llvm::dbgs());
          llvm::dbgs() << '\n';
        });

  return f;
}

void SILGenModule::postEmitFunction(SILDeclRef constant,
                                    SILFunction *F) {
  assert(!F->isExternalDeclaration() && "did not emit any function body?!");
  DEBUG(llvm::dbgs() << "lowered sil:\n";
        F->print(llvm::dbgs()));
  F->verify();
}

void SILGenModule::emitFunction(SILDeclRef::Loc decl, FuncExpr *fe) {
  // Emit any default argument generators.
  {
    auto patterns = fe->getArgParamPatterns();
    if (fe->getDecl() && fe->getDecl()->getDeclContext()->isTypeContext())
      patterns = patterns.slice(1);
    emitDefaultArgGenerators(decl, patterns);
  }

  // Ignore prototypes.
  if (fe->getBody() == nullptr) return;
  
  SILDeclRef constant(decl);
  SILFunction *f = preEmitFunction(constant, fe);
  bool hasVoidReturn = fe->getResultType(f->getASTContext())->isVoid();
  SILGenFunction(*this, *f, hasVoidReturn).emitFunction(fe);
  postEmitFunction(constant, f);

  // If the function is a standalone function and is curried, emit the thunks
  // for the intermediate curry levels.
  // FIXME: It might make more sense to do this lazily and emit curry thunks
  // with internal linkage.
  
  // Getters and setters can't be referenced uncurried, so skip thunking them.
  ValueDecl *vd = decl.dyn_cast<ValueDecl*>();
  FuncDecl *fd = dyn_cast_or_null<FuncDecl>(vd);
  if (fd && fd->isGetterOrSetter())
    return;
  
  // FIXME: Thunks for instance methods.
  if (fd && fd->isInstanceMember())
    return;
  
  // FIXME: Curry thunks for generic functions don't work right yet, so skip
  // emitting thunks for generic functions for now.
  if (f->getLoweredType().is<PolymorphicFunctionType>())
    return;
  
  // Generate the curry thunks.
  unsigned level = constant.uncurryLevel;
  while (level-- > 0) {
    SILDeclRef curryConstant = constant.atUncurryLevel(level);
    
    emitCurryThunk(curryConstant, constant, fe);
    constant = curryConstant;
  }
}

void SILGenModule::emitCurryThunk(SILDeclRef entryPoint,
                                  SILDeclRef nextEntryPoint,
                                  FuncExpr *fe) {
  SILFunction *f = preEmitFunction(entryPoint, fe);
  bool hasVoidReturn = fe->getResultType(f->getASTContext())->isVoid();
  SILGenFunction(*this, *f, hasVoidReturn)
    .emitCurryThunk(fe, entryPoint, nextEntryPoint);
  postEmitFunction(entryPoint, f);
}

void SILGenModule::addGlobalVariable(VarDecl *global) {
  M.globals.insert(global);
}

void SILGenModule::emitConstructor(ConstructorDecl *decl) {
  // Emit any default argument getter functions.
  emitDefaultArgGenerators(decl, decl->getArguments());

  SILDeclRef constant(decl);
  SILFunction *f = preEmitFunction(constant, decl);

  if (decl->getImplicitThisDecl()->getType()->getClassOrBoundGenericClass()) {
    // Class constructors have separate entry points for allocation and
    // initialization.
    SILGenFunction(*this, *f, /*hasVoidReturn=*/true)
      .emitClassConstructorAllocator(decl);
    postEmitFunction(constant, f);
    
    SILDeclRef initConstant(decl, SILDeclRef::Kind::Initializer);
    SILFunction *initF = preEmitFunction(initConstant, decl);
    SILGenFunction(*this, *initF, /*hasVoidReturn=*/true)
      .emitClassConstructorInitializer(decl);
    postEmitFunction(initConstant, initF);
  } else {
    // Struct constructors do everything in a single function.
    SILGenFunction(*this, *f, /*hasVoidReturn=*/true)
      .emitValueConstructor(decl);
    postEmitFunction(constant, f);
  }  
}

void SILGenModule::emitClosure(PipeClosureExpr *ce) {
  SILDeclRef constant(ce);
  SILFunction *f = preEmitFunction(constant, ce);
  bool hasVoidReturn = ce->getResultType()->isVoid();
  SILGenFunction(*this, *f, hasVoidReturn).emitClosure(ce);
  postEmitFunction(constant, f);
}

void SILGenModule::emitClosure(ClosureExpr *ce) {
  SILDeclRef constant(ce);
  SILFunction *f = preEmitFunction(constant, ce);
  SILGenFunction(*this, *f, /*hasVoidReturn=*/false).emitClosure(ce);
  postEmitFunction(constant, f);
}

void SILGenModule::emitDestructor(ClassDecl *cd,
                                  DestructorDecl /*nullable*/ *dd) {
  // Emit the destroying destructor.
  SILDeclRef destroyer(cd, SILDeclRef::Kind::Destroyer);
  SILFunction *f = preEmitFunction(destroyer, dd);
  SILGenFunction(*this, *f, /*hasVoidReturn=*/true).emitDestructor(cd, dd);
  postEmitFunction(destroyer, f);
}

void SILGenModule::emitDefaultArgGenerator(SILDeclRef constant, Expr *arg) {
  SILFunction *f = preEmitFunction(constant, arg);
  SILGenFunction(*this, *f, /*hasVoidReturn=*/arg->getType()->isVoid())
    .emitGeneratorFunction(constant, arg);
  postEmitFunction(constant, f);
}
  
void SILGenModule::emitDefaultArgGenerators(SILDeclRef::Loc decl,
                                            ArrayRef<Pattern*> patterns) {
  unsigned index = 0;
  for (auto pattern : patterns) {
    pattern = pattern->getSemanticsProvidingPattern();
    auto tuplePattern = dyn_cast<TuplePattern>(pattern);
    if (!tuplePattern) {
      ++index;
      continue;
    }

    for (auto &elt : tuplePattern->getFields()) {
      if (auto handle = elt.getInit()) {
        emitDefaultArgGenerator(SILDeclRef::getDefaultArgGenerator(decl,index),
                                handle->getExpr());
      }
      ++index;
    }
  }
}

void SILGenModule::emitObjCMethodThunk(FuncDecl *method) {
  SILDeclRef thunk(method, SILDeclRef::ConstructAtNaturalUncurryLevel,
                    /*isObjC*/ true);
  
  // Don't emit the thunk if it already exists.
  if (hasFunction(thunk))
    return;
  // TODO: why we have getBody here?
  SILFunction *f = preEmitFunction(thunk, method->getBody());
  SILGenFunction(*this, *f, false).emitObjCMethodThunk(thunk);
  postEmitFunction(thunk, f);
}

void SILGenModule::emitObjCPropertyMethodThunks(VarDecl *prop) {
  SILDeclRef getter(prop, SILDeclRef::Kind::Getter,
                     SILDeclRef::ConstructAtNaturalUncurryLevel,
                     /*isObjC*/ true);
                     
  // Don't emit the thunks if they already exist.
  if (hasFunction(getter))
    return;

  SILFunction *f = preEmitFunction(getter, prop);
  SILGenFunction(*this, *f, false).emitObjCPropertyGetter(getter);
  postEmitFunction(getter, f);

  if (!prop->isSettable())
    return;

  SILDeclRef setter(prop, SILDeclRef::Kind::Setter,
                     SILDeclRef::ConstructAtNaturalUncurryLevel,
                     /*isObjC*/ true);

  f = preEmitFunction(setter, prop);
  SILGenFunction(*this, *f, false).emitObjCPropertySetter(setter);
  postEmitFunction(setter, f);
}

void SILGenModule::visitPatternBindingDecl(PatternBindingDecl *pd) {
  // Emit initializers for variables in top-level code.
  // FIXME: Global initialization order?!
  if (TopLevelSGF) {
    if (!TopLevelSGF->B.hasValidInsertionPoint())
      return;
    
    TopLevelSGF->visit(pd);
  }
  
  // FIXME: generate accessor functions for global variables
}

void SILGenModule::visitVarDecl(VarDecl *vd) {
  if (!vd->isProperty())
    addGlobalVariable(vd);
}

//===--------------------------------------------------------------------===//
// SILModule::constructSIL method implementation
//===--------------------------------------------------------------------===//

SILModule *SILModule::constructSIL(TranslationUnit *tu,
                                   unsigned startElem) {
  SILModule *m = new SILModule(tu->getASTContext());
  SILGenModule sgm(*m);
  for (Decl *D : llvm::makeArrayRef(tu->Decls).slice(startElem))
    sgm.visit(D);
  
  // Emit external definitions used by this translation unit.
  for (auto def : tu->getASTContext().ExternalDefinitions) {
    sgm.emitExternalDefinition(def);
  }

  return m;
}

SILModule *swift::performSILGeneration(TranslationUnit *tu,
                                       unsigned startElem) {
  return SILModule::constructSIL(tu, startElem);
}
