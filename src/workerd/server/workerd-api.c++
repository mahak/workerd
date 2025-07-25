// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-api.h"

#include "workerd/api/worker-rpc.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/actor.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/base64.h>
#include <workerd/api/cache.h>
#include <workerd/api/commonjs.h>
#include <workerd/api/container.h>
#include <workerd/api/crypto/impl.h>
#include <workerd/api/encoding.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/filesystem.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/kv.h>
#include <workerd/api/memory-cache.h>
#include <workerd/api/modules.h>
#include <workerd/api/node/node.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/pyodide/requirements.h>
#include <workerd/api/pyodide/setup-emscripten.h>
#include <workerd/api/queue.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/r2.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/sql.h>
#include <workerd/api/streams.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/trace.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/urlpattern-standard.h>
#include <workerd/api/urlpattern.h>
#include <workerd/api/worker-loader.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/promise-wrapper.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/url.h>
#include <workerd/jsg/util.h>
#include <workerd/server/actor-id-impl.h>
#include <workerd/server/fallback-service.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/use-perfetto-categories.h>

#include <pyodide/generated/pyodide_extra.capnp.h>

#include <kj/compat/gzip.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/compat/url.h>

namespace workerd::server {

using api::pyodide::PythonConfig;

namespace {
JSG_DECLARE_ISOLATE_TYPE(JsgWorkerdIsolate,
    // Declares the listing of host object types and structs that the jsg
    // automatic type mapping will understand. Each of the various
    // NNNN_ISOLATE_TYPES macros are defined in different header files
    // (e.g. GLOBAL_SCOPE_ISOLATE_TYPES is defined in api/global-scope.h).
    //
    // Global scope types are defined first just by convention, the rest
    // of the list is in alphabetical order for easier readability (the
    // actual order of the items is unimportant), followed by additional
    // types defined in worker.c++ or as part of jsg.
    //
    // When adding a new NNNN_ISOLATE_TYPES macro, remember to add it to
    // src/workerd/api/rtti.c++ too (and tools/api-encoder.c++ for the
    // time being), so it gets included in the TypeScript types.
    EW_GLOBAL_SCOPE_ISOLATE_TYPES,

    EW_ACTOR_ISOLATE_TYPES,
    EW_ACTOR_STATE_ISOLATE_TYPES,
    EW_ANALYTICS_ENGINE_ISOLATE_TYPES,
    EW_BASE64_ISOLATE_TYPES,
    EW_BASICS_ISOLATE_TYPES,
    EW_BLOB_ISOLATE_TYPES,
    EW_CACHE_ISOLATE_TYPES,
    EW_CONTAINER_ISOLATE_TYPES,
    EW_CJS_ISOLATE_TYPES,
    EW_CRYPTO_ISOLATE_TYPES,
    EW_ENCODING_ISOLATE_TYPES,
    EW_EVENTS_ISOLATE_TYPES,
    EW_FORMDATA_ISOLATE_TYPES,
    EW_HTML_REWRITER_ISOLATE_TYPES,
    EW_HTTP_ISOLATE_TYPES,
    EW_SOCKETS_ISOLATE_TYPES,
    EW_KV_ISOLATE_TYPES,
    EW_PYODIDE_ISOLATE_TYPES,
    EW_QUEUE_ISOLATE_TYPES,
    EW_R2_PUBLIC_BETA_ADMIN_ISOLATE_TYPES,
    EW_R2_PUBLIC_BETA_ISOLATE_TYPES,
    EW_WORKER_RPC_ISOLATE_TYPES,
    EW_SCHEDULED_ISOLATE_TYPES,
    EW_STREAMS_ISOLATE_TYPES,
    EW_TRACE_ISOLATE_TYPES,
    EW_UNSAFE_ISOLATE_TYPES,
    EW_MEMORY_CACHE_ISOLATE_TYPES,
    EW_URL_ISOLATE_TYPES,
    EW_URL_STANDARD_ISOLATE_TYPES,
    EW_URLPATTERN_ISOLATE_TYPES,
    EW_URLPATTERN_STANDARD_ISOLATE_TYPES,
    EW_WEB_FILESYSTEM_ISOLATE_TYPE,
    EW_FILESYSTEM_ISOLATE_TYPES,
    EW_WEBSOCKET_ISOLATE_TYPES,
    EW_SQL_ISOLATE_TYPES,
    EW_NODE_ISOLATE_TYPES,
    EW_RTTI_ISOLATE_TYPES,
    EW_HYPERDRIVE_ISOLATE_TYPES,
    EW_EVENTSOURCE_ISOLATE_TYPES,
    EW_WORKER_LOADER_ISOLATE_TYPES,
    EW_MESSAGECHANNEL_ISOLATE_TYPES,
    workerd::api::EnvModule,

    jsg::TypeWrapperExtension<PromiseWrapper>,
    jsg::InjectConfiguration<CompatibilityFlags::Reader>,
    Worker::Api::ErrorInterface);

static const PythonConfig defaultConfig{
  .packageDiskCacheRoot = kj::none,
  .pyodideDiskCacheRoot = kj::none,
  .createSnapshot = false,
  .createBaselineSnapshot = false,
};

kj::Own<api::pyodide::PyodideMetadataReader::State> makePyodideMetadataReader(
    const Worker::Script::ModulesSource& source,
    const PythonConfig& pythonConfig,
    PythonSnapshotRelease::Reader pythonRelease) {
  auto modules = source.modules.asPtr();
  auto mainModule = kj::str(source.mainModule);
  int numFiles = 0;
  int numRequirements = 0;
  for (auto& module: modules) {
    KJ_SWITCH_ONEOF(module.content) {
      KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
        // Not exposeud to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
        // Not exposeud to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
        // Not exposeud to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
        numRequirements++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
        // Not exposeud to Python.
      }
    }
  }

  auto names = kj::heapArrayBuilder<kj::String>(numFiles);
  auto contents = kj::heapArrayBuilder<kj::Array<kj::byte>>(numFiles);
  auto requirements = kj::heapArrayBuilder<kj::String>(numRequirements);
  for (auto& module: modules) {
    KJ_SWITCH_ONEOF(module.content) {
      KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body.asBytes()));
      }
      KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body));
      }
      KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
        // Not exposeud to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body.asBytes()));
      }
      KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
        // Not exposeud to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
        // Not exposeud to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
        KJ_REQUIRE(module.name.endsWith(".py"));
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body.asBytes()));
      }
      KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
        requirements.add(kj::str(module.name));
      }
      KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
        // Not exposeud to Python.
      }
    }
  }
  bool snapshotToDisk = pythonConfig.createSnapshot || pythonConfig.createBaselineSnapshot;
  if (pythonConfig.loadSnapshotFromDisk && snapshotToDisk) {
    KJ_FAIL_ASSERT(
        "Doesn't make sense to pass both --python-save-snapshot and --python-load-snapshot");
  }
  kj::Maybe<kj::Array<kj::byte>> memorySnapshot = kj::none;
  if (pythonConfig.loadSnapshotFromDisk) {
    auto& root = KJ_REQUIRE_NONNULL(pythonConfig.packageDiskCacheRoot);
    kj::Path path("snapshot.bin");
    auto maybeFile = root->tryOpenFile(path);
    if (maybeFile == kj::none) {
      KJ_FAIL_REQUIRE("Expected to find snapshot.bin in the package cache directory");
    }
    memorySnapshot = KJ_REQUIRE_NONNULL(maybeFile)->readAllBytes();
  }
  auto lock = KJ_ASSERT_NONNULL(api::pyodide::getPyodideLock(pythonRelease),
      kj::str("No lock file defined for Python packages release ", pythonRelease.getPackages()));

  // clang-format off
  return kj::heap<api::pyodide::PyodideMetadataReader::State>(
    kj::mv(mainModule),
    names.finish(),
    contents.finish(),
    requirements.finish(),
    kj::str(pythonRelease.getPyodide()),
    kj::str(pythonRelease.getPackages()),
    kj::mv(lock),
    true      /* isWorkerd */,
    false     /* isTracing */,
    snapshotToDisk,
    pythonConfig.createBaselineSnapshot,
    kj::mv(memorySnapshot)
  );
  // clang-format on
}

// An ActorStorage implementation which will always respond to reads as if the state is empty,
// and will fail any writes.
class EmptyReadOnlyActorStorageImpl final: public rpc::ActorStorage::Stage::Server {
 public:
  kj::Promise<void> get(GetContext context) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> getMultiple(GetMultipleContext context) override {
    return context.getParams()
        .getStream()
        .endRequest(capnp::MessageSize{2, 0})
        .sendIgnoringResult();
  }
  kj::Promise<void> list(ListContext context) override {
    return context.getParams()
        .getStream()
        .endRequest(capnp::MessageSize{2, 0})
        .sendIgnoringResult();
  }
  kj::Promise<void> getAlarm(GetAlarmContext context) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> txn(TxnContext context) override {
    auto results = context.getResults(capnp::MessageSize{2, 1});
    results.setTransaction(kj::heap<TransactionImpl>());
    return kj::READY_NOW;
  }

 private:
  class TransactionImpl final: public rpc::ActorStorage::Stage::Transaction::Server {
   protected:
    kj::Promise<void> get(GetContext context) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> getMultiple(GetMultipleContext context) override {
      return context.getParams()
          .getStream()
          .endRequest(capnp::MessageSize{2, 0})
          .sendIgnoringResult();
    }
    kj::Promise<void> list(ListContext context) override {
      return context.getParams()
          .getStream()
          .endRequest(capnp::MessageSize{2, 0})
          .sendIgnoringResult();
    }
    kj::Promise<void> getAlarm(GetAlarmContext context) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> commit(CommitContext context) override {
      return kj::READY_NOW;
    }
  };
};

}  // namespace

jsg::Bundle::Reader retrievePyodideBundle(
    const api::pyodide::PythonConfig& pyConfig, kj::StringPtr version) {
  auto result = pyConfig.pyodideBundleManager.getPyodideBundle(version);
  return KJ_ASSERT_NONNULL(result, "Failed to get Pyodide bundle");
}

/**
 * This function matches the implementation of `getPythonRequirements` in the internal repo. But it
 * works on the workerd ModulesSource definition rather than the WorkerBundle.
 */
kj::Array<kj::String> getPythonRequirements(const Worker::Script::ModulesSource& source) {
  kj::Vector<kj::String> requirements;

  for (auto& def: source.modules) {
    KJ_SWITCH_ONEOF(def.content) {
      KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
        requirements.add(api::pyodide::canonicalizePythonPackageName(def.name));
      }
      KJ_CASE_ONEOF_DEFAULT {
        break;
      }
    }
  }

  return requirements.releaseAsArray();
}

struct WorkerdApi::Impl final {
  kj::Own<CompatibilityFlags::Reader> features;
  capnp::List<config::Extension>::Reader extensions;
  kj::Own<VirtualFileSystem> vfs;
  kj::Maybe<kj::Own<jsg::modules::ModuleRegistry>> maybeOwnedModuleRegistry;
  kj::Own<JsgIsolateObserver> observer;
  JsgWorkerdIsolate jsgIsolate;
  api::MemoryCacheProvider& memoryCacheProvider;
  const PythonConfig& pythonConfig;

  class Configuration {
   public:
    Configuration(Impl& impl)
        : features(*impl.features),
          jsgConfig(jsg::JsgConfig{
            .noSubstituteNull = features.getNoSubstituteNull(),
            .unwrapCustomThenables = features.getUnwrapCustomThenables(),
          }) {}
    operator const CompatibilityFlags::Reader() const {
      return features;
    }
    operator const jsg::JsgConfig&() const {
      return jsgConfig;
    }

   private:
    CompatibilityFlags::Reader& features;
    jsg::JsgConfig jsgConfig;
  };

  Impl(jsg::V8System& v8System,
      CompatibilityFlags::Reader featuresParam,
      capnp::List<config::Extension>::Reader extensionsParam,
      v8::Isolate::CreateParams createParams,
      v8::IsolateGroup group,
      kj::Own<JsgIsolateObserver> observerParam,
      api::MemoryCacheProvider& memoryCacheProvider,
      kj::Own<VirtualFileSystem> vfs,
      const PythonConfig& pythonConfig = defaultConfig,
      kj::Maybe<kj::Own<jsg::modules::ModuleRegistry>> newModuleRegistry = kj::none)
      : features(capnp::clone(featuresParam)),
        extensions(extensionsParam),
        vfs(kj::mv(vfs)),
        maybeOwnedModuleRegistry(kj::mv(newModuleRegistry)),
        observer(kj::atomicAddRef(*observerParam)),
        jsgIsolate(
            v8System, group, Configuration(*this), kj::mv(observerParam), kj::mv(createParams)),
        memoryCacheProvider(memoryCacheProvider),
        pythonConfig(pythonConfig) {
    jsgIsolate.runInLockScope([&](JsgWorkerdIsolate::Lock& lock) {
      // maybeOwnedModuleRegistry is only set when using the
      // new module registry implementation. When that is the
      // case we also need to tell JSG.
      if (maybeOwnedModuleRegistry != kj::none) {
        jsgIsolate.setUsingNewModuleRegistry();
      }
    });
  }

  static v8::Local<v8::String> compileTextGlobal(
      JsgWorkerdIsolate::Lock& lock, capnp::Text::Reader reader) {
    return lock.wrapNoContext(reader);
  };

  static v8::Local<v8::ArrayBuffer> compileDataGlobal(
      JsgWorkerdIsolate::Lock& lock, capnp::Data::Reader reader) {
    return lock.wrapNoContext(kj::heapArray(reader));
  };

  static v8::Local<v8::WasmModuleObject> compileWasmGlobal(JsgWorkerdIsolate::Lock& lock,
      capnp::Data::Reader reader,
      const jsg::CompilationObserver& observer) {
    lock.setAllowEval(true);
    KJ_DEFER(lock.setAllowEval(false));

    // Allow Wasm compilation to spawn a background thread for tier-up, i.e. recompiling
    // Wasm with optimizations in the background. Otherwise Wasm startup is way too slow.
    // Until tier-up finishes, requests will be handled using Liftoff-generated code, which
    // compiles fast but runs slower.
    AllowV8BackgroundThreadsScope scope;

    return jsg::compileWasmModule(lock, reader, observer);
  };

  static v8::Local<v8::Value> compileJsonGlobal(
      JsgWorkerdIsolate::Lock& lock, capnp::Text::Reader reader) {
    return jsg::check(v8::JSON::Parse(lock.v8Context(), lock.wrapNoContext(reader)));
  };

  kj::Maybe<jsg::modules::ModuleRegistry&> tryGetModuleRegistry() const {
    KJ_IF_SOME(owned, maybeOwnedModuleRegistry) {
      return *const_cast<jsg::modules::ModuleRegistry*>(owned.get());
    }
    return kj::none;
  }
};

WorkerdApi::WorkerdApi(jsg::V8System& v8System,
    CompatibilityFlags::Reader features,
    capnp::List<config::Extension>::Reader extensions,
    v8::Isolate::CreateParams createParams,
    v8::IsolateGroup group,
    kj::Own<JsgIsolateObserver> observer,
    api::MemoryCacheProvider& memoryCacheProvider,
    const PythonConfig& pythonConfig,
    kj::Maybe<kj::Own<jsg::modules::ModuleRegistry>> newModuleRegistry,
    kj::Own<VirtualFileSystem> vfs)
    : impl(kj::heap<Impl>(v8System,
          features,
          extensions,
          kj::mv(createParams),
          group,
          kj::mv(observer),
          memoryCacheProvider,
          kj::mv(vfs),
          pythonConfig,
          kj::mv(newModuleRegistry))) {}
WorkerdApi::~WorkerdApi() noexcept(false) {}

kj::Own<jsg::Lock> WorkerdApi::lock(jsg::V8StackScope& stackScope) const {
  return kj::heap<JsgWorkerdIsolate::Lock>(impl->jsgIsolate, stackScope);
}
CompatibilityFlags::Reader WorkerdApi::getFeatureFlags() const {
  return *impl->features;
}
jsg::JsContext<api::ServiceWorkerGlobalScope> WorkerdApi::newContext(jsg::Lock& lock) const {
  jsg::NewContextOptions options{
    .newModuleRegistry = impl->tryGetModuleRegistry(),
    .enableWeakRef = getFeatureFlags().getJsWeakRef(),
  };
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).newContext<api::ServiceWorkerGlobalScope>(
      kj::mv(options));
}
jsg::Dict<NamedExport> WorkerdApi::unwrapExports(
    jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).unwrap<jsg::Dict<NamedExport>>(
      lock.v8Context(), moduleNamespace);
}
NamedExport WorkerdApi::unwrapExport(jsg::Lock& lock, v8::Local<v8::Value> exportVal) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).unwrap<NamedExport>(
      lock.v8Context(), exportVal);
}
EntrypointClasses WorkerdApi::getEntrypointClasses(jsg::Lock& lock) const {
  auto& typedLock = kj::downcast<JsgWorkerdIsolate::Lock>(lock);

  return {
    .workerEntrypoint = typedLock.getConstructor<api::WorkerEntrypoint>(lock.v8Context()),
    .durableObject = typedLock.getConstructor<api::DurableObjectBase>(lock.v8Context()),
    .workflowEntrypoint = typedLock.getConstructor<api::WorkflowEntrypoint>(lock.v8Context()),
  };
}
const jsg::TypeHandler<Worker::Api::ErrorInterface>& WorkerdApi::getErrorInterfaceTypeHandler(
    jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).getTypeHandler<ErrorInterface>();
}

const jsg::TypeHandler<api::QueueExportedHandler>& WorkerdApi::getQueueTypeHandler(
    jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).getTypeHandler<api::QueueExportedHandler>();
}

jsg::JsObject WorkerdApi::wrapExecutionContext(
    jsg::Lock& lock, jsg::Ref<api::ExecutionContext> ref) const {
  return jsg::JsObject(
      kj::downcast<JsgWorkerdIsolate::Lock>(lock).wrap(lock.v8Context(), kj::mv(ref)));
}

const jsg::IsolateObserver& WorkerdApi::getObserver() const {
  return *impl->observer;
}

void WorkerdApi::setIsolateObserver(IsolateObserver&) {};

Worker::Script::Source WorkerdApi::extractSource(kj::StringPtr name,
    config::Worker::Reader conf,
    Worker::ValidationErrorReporter& errorReporter) {
  TRACE_EVENT("workerd", "WorkerdApi::extractSource()");
  switch (conf.which()) {
    case config::Worker::MODULES: {
      auto modules = conf.getModules();
      if (modules.size() == 0) {
        errorReporter.addError(kj::str("Modules list cannot be empty."));
        goto invalid;
      }

      bool isPython = false;
      auto moduleArray = KJ_MAP(module, modules) -> Worker::Script::Module {
        if (module.isPythonModule()) {
          isPython = true;
        }
        return readModuleConf(module, errorReporter);
      };

      Worker::Script::ModulesSource result{
        .mainModule = modules[0].getName(), .modules = kj::mv(moduleArray), .isPython = isPython};

      return result;
    }
    case config::Worker::SERVICE_WORKER_SCRIPT: {
      uint wasmCount = 0;
      for (auto binding: conf.getBindings()) {
        if (binding.isWasmModule()) ++wasmCount;
      }

      auto globals = kj::heapArrayBuilder<Worker::Script::Module>(wasmCount);
      for (auto binding: conf.getBindings()) {
        if (binding.isWasmModule()) {
          globals.add(Worker::Script::Module{.name = binding.getName(),
            .content = Worker::Script::WasmModule{.body = binding.getWasmModule()}});
        }
      }

      return Worker::Script::ScriptSource{
        .mainScript = conf.getServiceWorkerScript(),
        .mainScriptName = name,
        .globals = globals.finish(),
      };
    }
    case config::Worker::INHERIT:
      // TODO(beta): Support inherit.
      KJ_FAIL_ASSERT("inherit should have been handled earlier");
  }

  errorReporter.addError(kj::str("Encountered unknown Worker code type. Was the "
                                 "config compiled with a newer version of the schema?"));
invalid:
  return Worker::Script::ScriptSource{""_kj, name, nullptr};
}

kj::Array<Worker::Script::CompiledGlobal> WorkerdApi::compileServiceWorkerGlobals(
    jsg::Lock& lockParam,
    const Worker::Script::ScriptSource& source,
    const Worker::Isolate& isolate) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileScriptGlobals()");
  // For Service Worker scripts, we support Wasm modules as globals, but they need to be loaded
  // at script load time.

  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(lockParam);

  auto compiledGlobals =
      kj::heapArrayBuilder<Worker::Script::CompiledGlobal>(source.globals.size());
  for (auto& global: source.globals) {
    KJ_IF_SOME(wasm, global.content.tryGet<Worker::Script::WasmModule>()) {
      auto name = lock.str(global.name);
      auto value = Impl::compileWasmGlobal(lock, wasm.body, *impl->observer);

      compiledGlobals.add(Worker::Script::CompiledGlobal{
        {lock.v8Isolate, name},
        {lock.v8Isolate, value},
      });
    } else {
      JSG_FAIL_REQUIRE(Error, "Unsupported module type for Service Worker global: ", global.name);
    }
  }

  return compiledGlobals.finish();
}

// Part of the original module registry implementation.
kj::Maybe<jsg::ModuleRegistry::ModuleInfo> WorkerdApi::tryCompileModule(jsg::Lock& js,
    config::Worker::Module::Reader conf,
    jsg::CompilationObserver& observer,
    CompatibilityFlags::Reader featureFlags) {
  return tryCompileModule(js, readModuleConf(conf), observer, featureFlags);
}

kj::Maybe<jsg::ModuleRegistry::ModuleInfo> WorkerdApi::tryCompileModule(jsg::Lock& js,
    const Worker::Script::Module& module,
    jsg::CompilationObserver& observer,
    CompatibilityFlags::Reader featureFlags) {
  TRACE_EVENT("workerd", "WorkerdApi::tryCompileModule()", "name", module.name);
  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(js);
  KJ_SWITCH_ONEOF(module.content) {
    KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
      return jsg::ModuleRegistry::ModuleInfo(lock, module.name, kj::none,
          jsg::ModuleRegistry::TextModuleInfo(lock, Impl::compileTextGlobal(lock, content.body)));
    }
    KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
      return jsg::ModuleRegistry::ModuleInfo(lock, module.name, kj::none,
          jsg::ModuleRegistry::DataModuleInfo(
              lock, Impl::compileDataGlobal(lock, content.body).As<v8::ArrayBuffer>()));
    }
    KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
      return jsg::ModuleRegistry::ModuleInfo(lock, module.name, kj::none,
          jsg::ModuleRegistry::WasmModuleInfo(
              lock, Impl::compileWasmGlobal(lock, content.body, observer)));
    }
    KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
      return jsg::ModuleRegistry::ModuleInfo(lock, module.name, kj::none,
          jsg::ModuleRegistry::JsonModuleInfo(lock, Impl::compileJsonGlobal(lock, content.body)));
    }
    KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
      // TODO(soon): Make sure passing nullptr to compile cache is desired.
      return jsg::ModuleRegistry::ModuleInfo(lock, module.name, content.body,
          nullptr /* compile cache */, jsg::ModuleInfoCompileOption::BUNDLE, observer);
    }
    KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
      return jsg::ModuleRegistry::ModuleInfo(lock, module.name, content.namedExports,
          jsg::ModuleRegistry::CommonJsModuleInfo(lock, module.name, content.body,
              kj::heap<api::CommonJsImpl<JsgWorkerdIsolate::Lock>>(
                  lock, kj::Path::parse(module.name))));
    }
    KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
      // Nothing to do. Handled in compileModules.
      return kj::none;
    }
    KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
      // Nothing to do. Handled in compileModules.
      return kj::none;
    }
    KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
      KJ_FAIL_REQUIRE("capnp modules are not yet supported in workerd");
    }
  }
  KJ_UNREACHABLE;
}

Worker::Script::Module WorkerdApi::readModuleConf(config::Worker::Module::Reader conf,
    kj::Maybe<Worker::ValidationErrorReporter&> errorReporter) {
  return {.name = conf.getName(), .content = [&]() -> Worker::Script::ModuleContent {
    switch (conf.which()) {
      case config::Worker::Module::TEXT:
        return Worker::Script::TextModule{conf.getText()};
      case config::Worker::Module::DATA:
        return Worker::Script::DataModule{conf.getData()};
      case config::Worker::Module::WASM:
        return Worker::Script::WasmModule{conf.getWasm()};
      case config::Worker::Module::JSON:
        return Worker::Script::JsonModule{conf.getJson()};
      case config::Worker::Module::ES_MODULE:
        return Worker::Script::EsModule{conf.getEsModule()};
      case config::Worker::Module::COMMON_JS_MODULE: {
        Worker::Script::CommonJsModule result{.body = conf.getCommonJsModule()};
        if (conf.hasNamedExports()) {
          result.namedExports = KJ_MAP(name, conf.getNamedExports()) -> kj::StringPtr { return name; };
        }
        return result;
      }
      case config::Worker::Module::PYTHON_MODULE:
        return Worker::Script::PythonModule{conf.getPythonModule()};
      case config::Worker::Module::PYTHON_REQUIREMENT:
        return Worker::Script::PythonRequirement{};
      case config::Worker::Module::OBSOLETE: {
        // A non-supported or obsolete module type was configured
        KJ_FAIL_REQUIRE("Worker bundle specified an unsupported module type");
      }
    }

    KJ_IF_SOME(e, errorReporter) {
      e.addError(kj::str("Encountered unknown Worker.Module type. Was the "
                         "config compiled with a newer version of the schema?"));
      return Worker::Script::TextModule{""};
    } else {
      KJ_FAIL_REQUIRE("unknown module type", (uint)conf.which());
    }
  }()};
}

// Part of the original module registry implementation.
void WorkerdApi::compileModules(jsg::Lock& lockParam,
    const Worker::Script::ModulesSource& source,
    const Worker::Isolate& isolate,
    kj::Maybe<kj::Own<api::pyodide::ArtifactBundler_State>> artifacts,
    SpanParent parentSpan) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileModules()");
  lockParam.withinHandleScope([&] {
    auto modules = jsg::ModuleRegistryImpl<JsgWorkerdIsolate_TypeWrapper>::from(lockParam);

    using namespace workerd::api::pyodide;
    auto featureFlags = getFeatureFlags();
    if (source.isPython) {
      KJ_REQUIRE(featureFlags.getPythonWorkers(),
          "The python_workers compatibility flag is required to use Python.");
      auto pythonRelease = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
      auto version = getPythonBundleName(pythonRelease);
      auto bundle = retrievePyodideBundle(impl->pythonConfig, version);
      // Inject SetupEmscripten module
      {
        auto emscriptenRuntime =
            api::pyodide::EmscriptenRuntime::initialize(lockParam, true, bundle);
        modules->addBuiltinModule("internal:setup-emscripten",
            jsg::alloc<SetupEmscripten>(kj::mv(emscriptenRuntime)),
            workerd::jsg::ModuleRegistry::Type::INTERNAL);
      }

      // Inject Pyodide bundle
      modules->addBuiltinBundle(bundle, kj::none);
      // Inject pyodide bootstrap module (TODO: load this from the capnproto bundle?)
      {
        Worker::Script::Module module{
          .name = source.mainModule, .content = Worker::Script::EsModule{PYTHON_ENTRYPOINT}};

        auto info = tryCompileModule(lockParam, module, modules->getObserver(), featureFlags);
        auto path = kj::Path::parse(source.mainModule);
        modules->add(path, kj::mv(KJ_REQUIRE_NONNULL(info)));
      }

      // Inject metadata that the entrypoint module will read.
      modules->addBuiltinModule("pyodide-internal:runtime-generated/metadata",
          lockParam.alloc<PyodideMetadataReader>(
              makePyodideMetadataReader(source, impl->pythonConfig, pythonRelease)),
          jsg::ModuleRegistry::Type::INTERNAL);

      // Inject packages tar file
      modules->addBuiltinModule("pyodide-internal:packages_tar_reader", "export default { }"_kj,
          workerd::jsg::ModuleRegistry::Type::INTERNAL, {});
      // Inject artifact bundler.
      modules->addBuiltinModule("pyodide-internal:artifacts",
          lockParam.alloc<ArtifactBundler>(kj::mv(artifacts).orDefault(
              []() { return api::pyodide::ArtifactBundler::makeDisabledBundler(); })),
          jsg::ModuleRegistry::Type::INTERNAL);

      // Inject jaeger internal tracer in a disabled state (we don't have a use for it in workerd)
      modules->addBuiltinModule("pyodide-internal:internalJaeger",
          DisabledInternalJaeger::create(lockParam), jsg::ModuleRegistry::Type::INTERNAL);

      // Inject disk cache module
      modules->addBuiltinModule("pyodide-internal:disk_cache",
          lockParam.alloc<DiskCache>(impl->pythonConfig.packageDiskCacheRoot),
          jsg::ModuleRegistry::Type::INTERNAL);

      // Inject a (disabled) SimplePythonLimiter
      modules->addBuiltinModule("pyodide-internal:limiter",
          SimplePythonLimiter::makeDisabled(lockParam), jsg::ModuleRegistry::Type::INTERNAL);
    }

    for (auto& module: source.modules) {
      auto path = kj::Path::parse(module.name);
      auto maybeInfo = tryCompileModule(lockParam, module, modules->getObserver(), featureFlags);
      KJ_IF_SOME(info, maybeInfo) {
        modules->add(path, kj::mv(info));
      }
    }

    api::registerModules(*modules, featureFlags);

    for (auto extension: impl->extensions) {
      for (auto module: extension.getModules()) {
        modules->addBuiltinModule(module.getName(), module.getEsModule().asArray(),
            module.getInternal() ? jsg::ModuleRegistry::Type::INTERNAL
                                 : jsg::ModuleRegistry::Type::BUILTIN);
      }
    }
  });
}

static v8::Local<v8::Value> createBindingValue(JsgWorkerdIsolate::Lock& lock,
    const WorkerdApi::Global& global,
    CompatibilityFlags::Reader featureFlags,
    uint32_t ownerId,
    api::MemoryCacheProvider& memoryCacheProvider) {
  TRACE_EVENT("workerd", "WorkerdApi::createBindingValue()");
  using Global = WorkerdApi::Global;
  auto context = lock.v8Context();

  v8::Local<v8::Value> value;

  // When new binding types are created. If their value resolves to be a string
  // or a JSON stringified/stringifiable value, then it should be added to
  // process.env here as well, just like with Global::Json and kj::String
  // entries.
  //
  // It is important to understand the process.env is fundamentally different
  // from the existing bag of bindings. The keys and values on process.env are
  // fundamentally a Record<string, string>, where any value set on process.env
  // is coerced to a string. Having a separate object for process.env is the
  // easiest approach as opposed to wrapping the bindings/env with a proxy that
  // tries to abstract the details. If this ends up needing to change later then
  // as long as the observable behavior remains the same we can do so without
  // Yet Another Compat Flag.

  KJ_SWITCH_ONEOF(global.value) {
    KJ_CASE_ONEOF(json, Global::Json) {
      value = jsg::check(v8::JSON::Parse(context, lock.str(json.text)));
    }

    KJ_CASE_ONEOF(pipeline, Global::Fetcher) {
      value = lock.wrap(context,
          lock.alloc<api::Fetcher>(pipeline.channel,
              pipeline.requiresHost ? api::Fetcher::RequiresHostAndProtocol::YES
                                    : api::Fetcher::RequiresHostAndProtocol::NO,
              pipeline.isInHouse));
    }

    KJ_CASE_ONEOF(ns, Global::KvNamespace) {
      value = lock.wrap(context,
          lock.alloc<api::KvNamespace>(
              kj::Array<api::KvNamespace::AdditionalHeader>{}, ns.subrequestChannel));
    }

    KJ_CASE_ONEOF(r2, Global::R2Bucket) {
      value = lock.wrap(
          context, lock.alloc<api::public_beta::R2Bucket>(featureFlags, r2.subrequestChannel));
    }

    KJ_CASE_ONEOF(r2a, Global::R2Admin) {
      value = lock.wrap(
          context, lock.alloc<api::public_beta::R2Admin>(featureFlags, r2a.subrequestChannel));
    }

    KJ_CASE_ONEOF(ns, Global::QueueBinding) {
      value = lock.wrap(context, lock.alloc<api::WorkerQueue>(ns.subrequestChannel));
    }

    KJ_CASE_ONEOF(key, Global::CryptoKey) {
      api::SubtleCrypto::ImportKeyData keyData;
      KJ_SWITCH_ONEOF(key.keyData) {
        KJ_CASE_ONEOF(data, kj::Array<byte>) {
          keyData = kj::heapArray(data.asPtr());
        }
        KJ_CASE_ONEOF(json, Global::Json) {
          v8::Local<v8::String> str = lock.wrap(context, kj::mv(json.text));
          v8::Local<v8::Value> obj = jsg::check(v8::JSON::Parse(context, str));
          keyData = lock.unwrap<api::SubtleCrypto::ImportKeyData>(context, obj);
        }
      }

      v8::Local<v8::String> algoStr = lock.wrap(context, kj::mv(key.algorithm.text));
      v8::Local<v8::Value> algo = jsg::check(v8::JSON::Parse(context, algoStr));
      auto importKeyAlgo =
          lock.unwrap<kj::OneOf<kj::String, api::SubtleCrypto::ImportKeyAlgorithm>>(context, algo);

      jsg::Ref<api::CryptoKey> importedKey =
          api::SubtleCrypto().importKeySync(lock, key.format, kj::mv(keyData),
              api::interpretAlgorithmParam(kj::mv(importKeyAlgo)), key.extractable, key.usages);

      value = lock.wrap(context, kj::mv(importedKey));
    }

    KJ_CASE_ONEOF(cache, Global::MemoryCache) {
      value = lock.wrap(context,
          lock.alloc<api::MemoryCache>(
              api::SharedMemoryCache::Use(memoryCacheProvider.getInstance(cache.cacheId),
                  {
                    .maxKeys = cache.maxKeys,
                    .maxValueSize = cache.maxValueSize,
                    .maxTotalValueSize = cache.maxTotalValueSize,
                  })));
    }

    KJ_CASE_ONEOF(ns, Global::EphemeralActorNamespace) {
      value = lock.wrap(context, lock.alloc<api::ColoLocalActorNamespace>(ns.actorChannel));
    }

    KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
      value = lock.wrap(context,
          lock.alloc<api::DurableObjectNamespace>(
              ns.actorChannel, kj::heap<ActorIdFactoryImpl>(ns.uniqueKey)));
    }

    KJ_CASE_ONEOF(ae, Global::AnalyticsEngine) {
      // Use subrequestChannel as logfwdrChannel
      value = lock.wrap(context,
          lock.alloc<api::AnalyticsEngine>(
              ae.subrequestChannel, kj::str(ae.dataset), ae.version, ownerId));
    }

    KJ_CASE_ONEOF(text, kj::String) {
      value = lock.wrap(context, kj::mv(text));
    }

    KJ_CASE_ONEOF(data, kj::Array<byte>) {
      value = lock.wrap(context, kj::heapArray(data.asPtr()));
    }

    KJ_CASE_ONEOF(wrapped, Global::Wrapped) {
      auto moduleRegistry = jsg::ModuleRegistry::from(lock);
      auto moduleName = kj::Path::parse(wrapped.moduleName);

      // wrapped bindings can be produced by internal modules only
      KJ_IF_SOME(moduleInfo,
          moduleRegistry->resolve(
              lock, moduleName, kj::none, jsg::ModuleRegistry::ResolveOption::INTERNAL_ONLY)) {
        // obtain the module
        auto module = moduleInfo.module.getHandle(lock);
        jsg::instantiateModule(lock, module);

        // build env object with inner bindings
        auto env = v8::Object::New(lock.v8Isolate);
        for (const auto& innerBinding: wrapped.innerBindings) {
          lock.v8Set(env, innerBinding.name,
              createBindingValue(lock, innerBinding, featureFlags, ownerId, memoryCacheProvider));
        }

        // obtain exported function to call
        auto moduleNs = jsg::check(module->GetModuleNamespace()->ToObject(context));
        auto fn = lock.v8Get(moduleNs, wrapped.entrypoint);
        KJ_ASSERT(fn->IsFunction(), "Entrypoint is not a function", wrapped.entrypoint);

        // invoke the function, its result will be binding value
        v8::Local<v8::Value> arg = env.As<v8::Value>();
        value = jsg::check(v8::Function::Cast(*fn)->Call(context, context->Global(), 1, &arg));
      } else {
        KJ_LOG(
            ERROR, "wrapped binding module can't be resolved (internal modules only)", moduleName);
      }
    }
    KJ_CASE_ONEOF(hyperdrive, Global::Hyperdrive) {
      value = lock.wrap(context,
          lock.alloc<api::Hyperdrive>(hyperdrive.subrequestChannel, kj::str(hyperdrive.database),
              kj::str(hyperdrive.user), kj::str(hyperdrive.password), kj::str(hyperdrive.scheme)));
    }
    KJ_CASE_ONEOF(unsafe, Global::UnsafeEval) {
      value = lock.wrap(context, lock.alloc<api::UnsafeEval>());
    }

    KJ_CASE_ONEOF(actorClass, Global::ActorClass) {
      value = lock.wrap(context, lock.alloc<api::DurableObjectClass>(actorClass.channel));
    }

    KJ_CASE_ONEOF(workerLoader, Global::WorkerLoader) {
      value = lock.wrap(context,
          lock.alloc<api::WorkerLoader>(
              workerLoader.channel, CompatibilityDateValidation::CODE_VERSION));
    }
  }

  return value;
}

void WorkerdApi::compileGlobals(jsg::Lock& lockParam,
    kj::ArrayPtr<const Global> globals,
    v8::Local<v8::Object> target,
    uint32_t ownerId) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileGlobals()");
  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(lockParam);
  lockParam.withinHandleScope([&] {
    auto& featureFlags = *impl->features;

    for (auto& global: globals) {
      lockParam.withinHandleScope([&] {
        // Don't use String's usual TypeHandler here because we want to intern the string.
        auto value =
            createBindingValue(lock, global, featureFlags, ownerId, impl->memoryCacheProvider);
        KJ_ASSERT(!value.IsEmpty(), "global did not produce v8::Value");
        lockParam.v8Set(target, global.name, value);
      });
    }
  });
}

void WorkerdApi::setModuleFallbackCallback(kj::Function<ModuleFallbackCallback>&& callback) const {
  auto& isolateBase = const_cast<JsgWorkerdIsolate&>(impl->jsgIsolate);
  isolateBase.setModuleFallbackCallback(kj::mv(callback));
}

// =======================================================================================

WorkerdApi::Global WorkerdApi::Global::clone() const {
  Global result;
  result.name = kj::str(name);

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(json, Global::Json) {
      result.value = json.clone();
    }
    KJ_CASE_ONEOF(fetcher, Global::Fetcher) {
      result.value = fetcher.clone();
    }
    KJ_CASE_ONEOF(kvNamespace, Global::KvNamespace) {
      result.value = kvNamespace.clone();
    }
    KJ_CASE_ONEOF(r2Bucket, Global::R2Bucket) {
      result.value = r2Bucket.clone();
    }
    KJ_CASE_ONEOF(r2Admin, Global::R2Admin) {
      result.value = r2Admin.clone();
    }
    KJ_CASE_ONEOF(queueBinding, Global::QueueBinding) {
      result.value = queueBinding.clone();
    }
    KJ_CASE_ONEOF(key, Global::CryptoKey) {
      result.value = key.clone();
    }
    KJ_CASE_ONEOF(cache, Global::MemoryCache) {
      result.value = cache.clone();
    }
    KJ_CASE_ONEOF(ns, Global::EphemeralActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(ae, Global::AnalyticsEngine) {
      result.value = ae.clone();
    }
    KJ_CASE_ONEOF(text, kj::String) {
      result.value = kj::str(text);
    }
    KJ_CASE_ONEOF(data, kj::Array<byte>) {
      result.value = kj::heapArray(data.asPtr());
    }
    KJ_CASE_ONEOF(wrapped, Global::Wrapped) {
      result.value = wrapped.clone();
    }
    KJ_CASE_ONEOF(hyperdrive, Global::Hyperdrive) {
      result.value = hyperdrive.clone();
    }
    KJ_CASE_ONEOF(unsafe, Global::UnsafeEval) {
      result.value = Global::UnsafeEval{};
    }

    KJ_CASE_ONEOF(actorClass, Global::ActorClass) {
      result.value = actorClass.clone();
    }
    KJ_CASE_ONEOF(workerLoader, Global::WorkerLoader) {
      result.value = workerLoader.clone();
    }
  }

  return result;
}

const WorkerdApi& WorkerdApi::from(const Worker::Api& api) {
  return kj::downcast<const WorkerdApi>(api);
}

// =======================================================================================

namespace {
static constexpr auto PYTHON_TAR_READER = "export default { }"_kj;
}  // namespace

kj::Own<jsg::modules::ModuleRegistry> WorkerdApi::initializeBundleModuleRegistry(
    const jsg::ResolveObserver& observer,
    kj::Maybe<const Worker::Script::ModulesSource&> maybeSource,
    const CompatibilityFlags::Reader& featureFlags,
    const PythonConfig& pythonConfig,
    const jsg::Url& bundleBase,
    capnp::List<config::Extension>::Reader extensions,
    kj::Maybe<kj::String> maybeFallbackService,
    kj::Maybe<kj::Own<api::pyodide::ArtifactBundler_State>> artifacts) {
  jsg::modules::ModuleRegistry::Builder builder(
      observer, bundleBase, jsg::modules::ModuleRegistry::Builder::Options::ALLOW_FALLBACK);

  // This callback is used when a module is being loaded to arrange evaluating the
  // module outside of the current IoContext.
  builder.setEvalCallback([](jsg::Lock& js, const auto& module, auto v8Module,
                              const auto& observer) -> jsg::Promise<jsg::Value> {
    return js.tryOrReject<jsg::Value>([&] {
      // Creating the SuppressIoContextScope here ensures that the current IoContext,
      // if any, is moved out of the way while we are evaluating.
      SuppressIoContextScope suppressIoContextScope;
      KJ_DASSERT(!IoContext::hasCurrent(), "Module evaluation must not be in an IoContext");
      return jsg::check(v8Module->Evaluate(js.v8Context()));
    });
  });

  // Add the module bundles that are built into to runtime.
  api::registerBuiltinModules<JsgWorkerdIsolate_TypeWrapper>(builder, featureFlags);

  // Add the module bundles that are configured by the worker (if any)
  // The only case where maybeSource is none is when the worker is using
  // the old service worker script format or "inherit", in which case
  // we will initialize a module registry with the built-ins, extensions,
  // etc but no worker bundle modules will be added.
  KJ_IF_SOME(source, maybeSource) {
    jsg::modules::ModuleBundle::BundleBuilder bundleBuilder(bundleBase);
    bool firstEsm = true;
    bool hasPythonModules = false;
    using namespace workerd::api::pyodide;

    for (auto& def: source.modules) {
      KJ_SWITCH_ONEOF(def.content) {
        KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
          jsg::modules::Module::Flags flags = jsg::modules::Module::Flags::ESM;
          // Only the first ESM module we encounter is the main module.
          // This should also be the first module in the list but we're
          // not enforcing that here.
          if (firstEsm) {
            flags = flags | jsg::modules::Module::Flags::MAIN;
            firstEsm = false;
          }
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addEsmModule(def.name, content.body, flags);
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newTextModuleHandler(content.body));
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newDataModuleHandler(content.body));
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newWasmModuleHandler(content.body));
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newJsonModuleHandler(content.body));
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
          kj::ArrayPtr<const kj::StringPtr> named;
          KJ_IF_SOME(n, content.namedExports) {
            named = n;
          }
          bundleBuilder.addSyntheticModule(def.name,
              jsg::modules::Module::newCjsStyleModuleHandler<api::CommonJsModuleContext,
                  JsgWorkerdIsolate_TypeWrapper>(content.body, def.name),
              KJ_MAP(name, named) { return kj::str(name); });
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
          KJ_REQUIRE(featureFlags.getPythonWorkers(),
              "The python_workers compatibility flag is required to use Python.");
          firstEsm = false;
          hasPythonModules = true;
          kj::StringPtr entry = PYTHON_ENTRYPOINT;
          bundleBuilder.addEsmModule(def.name, entry);
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
          // Handled separately
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
          KJ_FAIL_REQUIRE("capnp modules are not yet supported in workerd");
        }
      }
    }

    builder.add(bundleBuilder.finish());

    // Add the built-in module bundles that support python workers/pyodide.
    if (hasPythonModules) {
      // TODO(new-module-registry): Move into pyodide.h/pyodide.c++
      const auto bootrapSpecifier = "internal:setup-emscripten"_url;
      const auto metadataSpecifier = "pyodide-internal:runtime-generated/metadata"_url;
      const auto artifactsSpecifier = "pyodide-internal:artifacts"_url;
      const auto internalJaegerSpecifier = "pyodide-internal:internalJaeger"_url;
      const auto diskCacheSpecifier = "pyodide-internal:disk_cache"_url;
      const auto limiterSpecifier = "pyodide-internal:limiter"_url;
      const auto tarReaderSpecifier = "pyodide-internal:packages_tar_reader"_url;

      // To support python workers we create two modules bundles, one BUILTIN
      // and the other BUILTIN_ONLY. The BUILTIN bundle contains support modules
      // that need to be importable by the python worker bootstrap module (which
      // is added to the BUNDLE modules). The BUILTIN_ONLY bundle contains support
      // modules that are used by the BUILTIN modules and are not intended to be
      // accessible from the worker itself.

      // Inject metadata that the entrypoint module will read.
      auto pythonRelease = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
      auto version = getPythonBundleName(pythonRelease);
      auto bundle = retrievePyodideBundle(pythonConfig, version);

      // We end up add modules from the bundle twice, once to get BUILTIN modules
      // and again to get the BUILTIN_ONLY modules. These end up in two different
      // module bundles.
      jsg::modules::ModuleBundle::BuiltinBuilder pyodideSdkBuilder;

      // There are two bundles that are relevant here, PYODIDE_BUNDLE, which is
      // fixed and contains compiled-in modules, and the bundle that is fetched
      // that contains the more dynamic implementation details. We have to process
      // both.
      jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideSdkBuilder, PYODIDE_BUNDLE);
      jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideSdkBuilder, bundle);
      builder.add(pyodideSdkBuilder.finish());

      jsg::modules::ModuleBundle::BuiltinBuilder pyodideBundleBuilder(
          jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

      jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideBundleBuilder, PYODIDE_BUNDLE);
      jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideBundleBuilder, bundle);

      pyodideBundleBuilder.addSynthetic(bootrapSpecifier,
          jsg::modules::Module::newJsgObjectModuleHandler<api::pyodide::SetupEmscripten,
              JsgWorkerdIsolate_TypeWrapper>(
              [bundle](jsg::Lock& js) mutable -> jsg::Ref<api::pyodide::SetupEmscripten> {
        auto emscriptenRuntime = api::pyodide::EmscriptenRuntime::initialize(js, true, bundle);
        return js.alloc<api::pyodide::SetupEmscripten>(kj::mv(emscriptenRuntime));
      }));

      pyodideBundleBuilder.addEsm(tarReaderSpecifier, PYTHON_TAR_READER);

      pyodideBundleBuilder.addSynthetic(metadataSpecifier,
          jsg::modules::Module::newJsgObjectModuleHandler<api::pyodide::PyodideMetadataReader,
              JsgWorkerdIsolate_TypeWrapper>(
              [state = makePyodideMetadataReader(source, pythonConfig, pythonRelease)](
                  jsg::Lock& js) mutable -> jsg::Ref<api::pyodide::PyodideMetadataReader> {
        // The ModuleRegistry may be shared across multiple isolates and workers.
        // We need to clone the PyodideMetadataReader::State for each instance
        // that is evaluated. Typically this is only once per python worker
        // but could be more in the future.
        return js.alloc<PyodideMetadataReader>(state->clone());
      }));
      // Inject artifact bundler.
      pyodideBundleBuilder.addSynthetic(artifactsSpecifier,
          jsg::modules::Module::newJsgObjectModuleHandler<ArtifactBundler,
              JsgWorkerdIsolate_TypeWrapper>(
              [](jsg::Lock& js) mutable -> jsg::Ref<ArtifactBundler> {
        return js.alloc<ArtifactBundler>(ArtifactBundler::makeDisabledBundler());
      }));
      // Inject jaeger internal tracer in a disabled state (we don't have a use for it in workerd)
      pyodideBundleBuilder.addSynthetic(internalJaegerSpecifier,
          jsg::modules::Module::newJsgObjectModuleHandler<DisabledInternalJaeger,
              JsgWorkerdIsolate_TypeWrapper>(
              [](jsg::Lock& js) mutable -> jsg::Ref<DisabledInternalJaeger> {
        return DisabledInternalJaeger::create(js);
      }));
      // Inject disk cache module
      pyodideBundleBuilder.addSynthetic(diskCacheSpecifier,
          jsg::modules::Module::newJsgObjectModuleHandler<DiskCache, JsgWorkerdIsolate_TypeWrapper>(
              [&packageDiskCacheRoot = pythonConfig.packageDiskCacheRoot](jsg::Lock& js) mutable
              -> jsg::Ref<DiskCache> { return js.alloc<DiskCache>(packageDiskCacheRoot); }));
      // Inject a (disabled) SimplePythonLimiter
      pyodideBundleBuilder.addSynthetic(limiterSpecifier,
          jsg::modules::Module::newJsgObjectModuleHandler<SimplePythonLimiter,
              JsgWorkerdIsolate_TypeWrapper>(
              [](jsg::Lock& js) mutable -> jsg::Ref<SimplePythonLimiter> {
        return SimplePythonLimiter::makeDisabled(js);
      }));

      builder.add(pyodideBundleBuilder.finish());
    }
  }

  // Handle extensions
  jsg::modules::ModuleBundle::BuiltinBuilder publicExtensionsBuilder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  jsg::modules::ModuleBundle::BuiltinBuilder privateExtensionsBuilder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

  for (auto extension: extensions) {
    for (auto module: extension.getModules()) {
      KJ_IF_SOME(url, jsg::Url::tryParse(module.getName())) {
        if (module.getInternal()) {
          privateExtensionsBuilder.addEsm(url, module.getEsModule().asArray());
        } else {
          privateExtensionsBuilder.addEsm(url, module.getEsModule().asArray());
        }
      } else {
        KJ_LOG(WARNING, "Ignoring extension module with invalid name", module.getName());
      }
    }
  }

  builder.add(publicExtensionsBuilder.finish());
  builder.add(privateExtensionsBuilder.finish());

  // If we have a fallback service configured, add the fallback bundle.
  // The fallback bundle is used only in workerd local development mode.
  // If a module is not found in the static bundles, a registry that is
  // configured to use the fallback will send a request to the fallback
  // service to try resolving.
  KJ_IF_SOME(fallbackService, maybeFallbackService) {
    builder.add(jsg::modules::ModuleBundle::newFallbackBundle(
        [fallbackService = kj::str(fallbackService)](const jsg::modules::ResolveContext& context)
            -> kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>> {
      auto specifier = kj::str(context.specifier.getHref());
      auto referrer = kj::str(context.referrer.getHref());
      KJ_IF_SOME(resolved,
          workerd::fallback::tryResolve(workerd::fallback::Version::V2,
              workerd::fallback::ImportType::IMPORT, fallbackService, specifier,
              context.rawSpecifier.orDefault(nullptr), referrer, context.attributes)) {
        KJ_SWITCH_ONEOF(resolved) {
          KJ_CASE_ONEOF(str, kj::String) {
            // The fallback service returned an alternative specifier.
            // The resolution must start over with the new specifier.
            return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(kj::mv(str));
          }
          KJ_CASE_ONEOF(def, kj::Own<server::config::Worker::Module::Reader>) {
            // The fallback service returned a module definition.
            // We need to convert that into a Module instance.
            auto mod = readModuleConf(*def, kj::none);
            KJ_IF_SOME(specifier, jsg::Url::tryParse(mod.name)) {
              // Note that unlike the regular case, the module content returned
              // by the fallback service is not guaranteed to be memory-resident.
              // We need to copy the content into a heap-allocated arrays and
              // make sure those stay alive while the Module is alive.
              KJ_SWITCH_ONEOF(mod.content) {
                KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
                  return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                      jsg::modules::Module::newEsm(kj::mv(specifier),
                          jsg::modules::Module::Type::FALLBACK,
                          kj::heapArray<const char>(content.body)));
                }
                KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
                  auto ownedData = kj::str(content.body);
                  auto ptr = ownedData.asPtr();
                  return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                      jsg::modules::Module::newSynthetic(kj::mv(specifier),
                          jsg::modules::Module::Type::FALLBACK,
                          jsg::modules::Module::newTextModuleHandler(ptr))
                          .attach(kj::mv(ownedData)));
                }
                KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
                  auto ownedData = kj::heapArray<uint8_t>(content.body);
                  auto ptr = ownedData.asPtr();
                  return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                      jsg::modules::Module::newSynthetic(kj::mv(specifier),
                          jsg::modules::Module::Type::FALLBACK,
                          jsg::modules::Module::newDataModuleHandler(ptr))
                          .attach(kj::mv(ownedData)));
                }
                KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
                  auto ownedData = kj::heapArray<uint8_t>(content.body);
                  auto ptr = ownedData.asPtr();
                  return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                      jsg::modules::Module::newSynthetic(kj::mv(specifier),
                          jsg::modules::Module::Type::FALLBACK,
                          jsg::modules::Module::newWasmModuleHandler(ptr))
                          .attach(kj::mv(ownedData)));
                }
                KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
                  auto ownedData = kj::heapArray<const char>(content.body);
                  auto ptr = ownedData.asPtr();
                  return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                      jsg::modules::Module::newSynthetic(kj::mv(specifier),
                          jsg::modules::Module::Type::FALLBACK,
                          jsg::modules::Module::newJsonModuleHandler(ptr))
                          .attach(kj::mv(ownedData)));
                }
                KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
                  auto ownedData = kj::str(content.body);
                  auto ptr = ownedData.asPtr();
                  kj::ArrayPtr<const kj::StringPtr> named;
                  KJ_IF_SOME(n, content.namedExports) {
                    named = n;
                  }
                  return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                      jsg::modules::Module::newSynthetic(kj::mv(specifier),
                          jsg::modules::Module::Type::FALLBACK,
                          jsg::modules::Module::newCjsStyleModuleHandler<api::CommonJsModuleContext,
                              JsgWorkerdIsolate_TypeWrapper>(ptr, mod.name),
              KJ_MAP(name, named) {
                    return kj::str(name);
                  }).attach(kj::mv(ownedData)));
                }
                KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
                  // Python modules are not supported.in fallback
                  KJ_LOG(WARNING, "Fallback service returned a Python module");
                  return kj::none;
                }
                KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
                  // Python requirement modules are not supported.in fallback
                  KJ_LOG(WARNING, "Fallback service returned a Python requirement");
                  return kj::none;
                }
                KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
                  // Capnp modules are not supported.in fallback
                  KJ_LOG(WARNING, "Fallback service returned a Capnp module");
                  return kj::none;
                }
              }
              KJ_UNREACHABLE;
            }
            KJ_LOG(WARNING, "Fallback service returned an invalid specifier");
            return kj::none;
          }
        }
      }
      return kj::none;
    }));
  }

  // All done!
  return builder.finish();
}

const VirtualFileSystem& WorkerdApi::getVirtualFileSystem() const {
  return *impl->vfs;
}

kj::Own<rpc::ActorStorage::Stage::Server> newEmptyReadOnlyActorStorage() {
  return kj::heap<EmptyReadOnlyActorStorageImpl>();
}

}  // namespace workerd::server
