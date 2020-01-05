//===--- ClangModuleDependencyScanner.cpp - Dependency Scanning -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements dependency scanning for Clang modules.
//
//===----------------------------------------------------------------------===//
#include "ImporterImpl.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangImporterOptions.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Signals.h"

using namespace swift;

using namespace clang::tooling;
using namespace clang::tooling::dependencies;

class swift::ClangModuleDependenciesCacheImpl {
  /// The name of the file used for the "import hack" to compute module
  /// dependencies.
  /// FIXME: This should go away once Clang's dependency scanning library
  /// can scan by module name.
  std::string importHackFile;

public:
  /// Set containing all of the Clang modules that have already been seen.
  llvm::StringSet<> alreadySeen;

  DependencyScanningService service;

  DependencyScanningTool tool;

  ClangModuleDependenciesCacheImpl()
      : service(ScanningMode::MinimizedSourcePreprocessing,
                ScanningOutputFormat::Full),
        tool(service) { }
  ~ClangModuleDependenciesCacheImpl();

  /// Retrieve the name of the file used for the "import hack" that is
  /// used to scan the dependencies of a Clang module.
  llvm::ErrorOr<StringRef> getImportHackFile();
};

ClangModuleDependenciesCacheImpl::~ClangModuleDependenciesCacheImpl() {
  if (!importHackFile.empty()) {
    llvm::sys::fs::remove(importHackFile);
  }
}

llvm::ErrorOr<StringRef> ClangModuleDependenciesCacheImpl::getImportHackFile() {
  if (!importHackFile.empty())
    return importHackFile;

  // Create a temporary file.
  int resultFD;
  SmallString<128> resultPath;
  if (auto error = llvm::sys::fs::createTemporaryFile(
          "import-hack", "m", resultFD, resultPath))
    return error;

  llvm::raw_fd_ostream out(resultFD, /*shouldClose=*/true);
  out << "@import HACK_MODULE_NAME;\n";
  llvm::sys::RemoveFileOnSignal(resultPath);
  importHackFile = resultPath.str().str();
  return importHackFile;
}

namespace {
  class SingleCommandCompilationDatabase : public CompilationDatabase {
  public:
    SingleCommandCompilationDatabase(CompileCommand Cmd)
        : Command(std::move(Cmd)) {}

    virtual std::vector<CompileCommand>
    getCompileCommands(StringRef FilePath) const {
      return {Command};
    }

    virtual std::vector<CompileCommand> getAllCompileCommands() const {
      return {Command};
    }

  private:
    CompileCommand Command;
  };
}

Optional<ModuleDependencies> ClangImporter::getModuleDependencies(
    StringRef moduleName, ModuleDependenciesCache &cache) {
  // Check whether there is already a cached result.
  if (auto found = cache.findDependencies(
          moduleName, ModuleDependenciesKind::Clang))
    return found;

  // Retrieve or create the shared state.
  auto clangImpl = cache.getClangImpl();
  if (!clangImpl) {
    clangImpl = new ClangModuleDependenciesCacheImpl();
    cache.setClangImpl(clangImpl,
                       [](ClangModuleDependenciesCacheImpl *ptr) {
      delete ptr;
    });
  }

  // Reform the Clang importer options.
  // FIXME: Just save a reference or copy so we can get this back.
  ClangImporterOptions importerOpts;

  // Determine the command-line arguments for dependency scanning.
  auto &ctx = Impl.SwiftContext;
  std::vector<std::string> commandLineArgs;
  commandLineArgs.push_back("clang");
  importer::getNormalInvocationArguments(commandLineArgs, ctx, importerOpts);
  importer::addCommonInvocationArguments(commandLineArgs, ctx, importerOpts);

  // Add search paths.
  // Note: This is handled differently for the Clang importer itself, which
  // adds search paths to Clang's data structures rather than to its
  // command line.
  SearchPathOptions &searchPathOpts = ctx.SearchPathOpts;
  for (const auto &framepath : searchPathOpts.FrameworkSearchPaths) {
    commandLineArgs.push_back(framepath.IsSystem ? "-iframework" : "-F");
    commandLineArgs.push_back(framepath.Path);
  }

  for (auto path : searchPathOpts.ImportSearchPaths) {
    commandLineArgs.push_back("-I");
    commandLineArgs.push_back(path);
  }

  // HACK! Replace the module import buffer name with the source file hack.
  auto importHackFile = clangImpl->getImportHackFile();
  if (!importHackFile) {
    // FIXME: Emit a diagnostic here.
    return None;
  }

  auto sourceFilePos = std::find(
      commandLineArgs.begin(), commandLineArgs.end(),
      "<swift-imported-modules>");
  assert(sourceFilePos != commandLineArgs.end());
  *sourceFilePos = *importHackFile;

  // HACK! Drop the -fmodule-format= argument and the one that
  // precedes it.
  {
    auto moduleFormatPos = std::find_if(commandLineArgs.begin(),
                                        commandLineArgs.end(),
                                        [](StringRef arg) {
      return arg.startswith("-fmodule-format=");
    });
    assert(moduleFormatPos != commandLineArgs.end());
    assert(moduleFormatPos != commandLineArgs.begin());
    commandLineArgs.erase(moduleFormatPos-1, moduleFormatPos+1);
  }

  // HACK: No -fsyntax-only here?
  {
    auto syntaxOnlyPos = std::find(commandLineArgs.begin(),
                                   commandLineArgs.end(),
                                   "-fsyntax-only");
    assert(syntaxOnlyPos != commandLineArgs.end());
    *syntaxOnlyPos = "-c";
  }

  // HACK: Stolen from ClangScanDeps.cpp
  commandLineArgs.push_back("-o");
  commandLineArgs.push_back("/dev/null");
  commandLineArgs.push_back("-M");
  commandLineArgs.push_back("-MT");
  commandLineArgs.push_back("import-hack.o");
  commandLineArgs.push_back("-Xclang");
  commandLineArgs.push_back("-Eonly");
  commandLineArgs.push_back("-Xclang");
  commandLineArgs.push_back("-sys-header-deps");
  commandLineArgs.push_back("-Wno-error");

  // HACK! Trick out a .m file to use to import the module we name.
  std::string moduleNameHackDefine =
      ("-DHACK_MODULE_NAME=" + moduleName).str();
  commandLineArgs.push_back(moduleNameHackDefine);
  commandLineArgs.push_back("-fmodules-ignore-macro=HACK_MODULE_NAME");

  std::string workingDir =
      ctx.SourceMgr.getFileSystem()->getCurrentWorkingDirectory().get();
  CompileCommand command(workingDir, *importHackFile, commandLineArgs, "-");
  SingleCommandCompilationDatabase database(command);

  auto clangDependencies = clangImpl->tool.getFullDependencies(
      database, workingDir, clangImpl->alreadySeen);

  if (!clangDependencies) {
    // FIXME: Route this to a normal diagnostic.
    llvm::logAllUnhandledErrors(clangDependencies.takeError(), llvm::errs());
    return None;
  }

  // Record module dependencies for each module we found.
  llvm::StringSet<> alreadyAddedModules;
  for (const auto &clangModuleDep : clangDependencies->DiscoveredModules) {
    // If we've already cached this information, we're done.
    if (cache.hasDependencies(clangModuleDep.ModuleName,
                              ModuleDependenciesKind::Clang))
      continue;

    std::vector<std::string> fileDeps;
    for (const auto &fileDep : clangModuleDep.FileDeps) {
      fileDeps.push_back(fileDep.getKey());
    }

    // Create a module filename.
    // FIXME: Query Clang to determine an appropriate hashed name for the
    // module file.
    llvm::SmallString<32> modulePath = moduleName;
    llvm::sys::path::replace_extension(modulePath, "pcm");

    auto dependencies = ModuleDependencies::forClangModule(
        modulePath.str(), clangModuleDep.ClangModuleMapFile, fileDeps);
    for (const auto &moduleName : clangModuleDep.ClangModuleDeps) {
      dependencies.addModuleDependency(moduleName.ModuleName, alreadyAddedModules);
    }

    cache.recordDependencies(clangModuleDep.ModuleName,
                             std::move(dependencies),
                             ModuleDependenciesKind::Clang);
  }

  return cache.findDependencies(moduleName, ModuleDependenciesKind::Clang);
}
