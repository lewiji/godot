/*************************************************************************/
/*  gd_mono.cpp                                                          */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "gd_mono.h"

#include "core/config/project_settings.h"
#include "core/debugger/engine_debugger.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/thread.h"

#include "../csharp_script.h"
#include "../godotsharp_dirs.h"
#include "../utils/path_utils.h"
#include "gd_mono_cache.h"

#include <nethost.h>

#include <coreclr_delegates.h>
#include <hostfxr.h>

#warning TODO mobile
#if 0
#ifdef ANDROID_ENABLED
#include "android_mono_config.h"
#include "support/android_support.h"
#elif defined(IOS_ENABLED)
#include "support/ios_support.h"
#endif
#endif

GDMono *GDMono::singleton = nullptr;

namespace {

#warning "TODO .NET debugging and profiling. What's needed?"
#if 0
void gd_mono_profiler_init() {
	String profiler_args = GLOBAL_DEF("mono/profiler/args", "log:calls,alloc,sample,output=output.mlpd");
	bool profiler_enabled = GLOBAL_DEF("mono/profiler/enabled", false);
	if (profiler_enabled) {
		mono_profiler_load(profiler_args.utf8());
		return;
	}

	const String env_var_name = "MONO_ENV_OPTIONS";
	if (OS::get_singleton()->has_environment(env_var_name)) {
		const String mono_env_ops = OS::get_singleton()->get_environment(env_var_name);
		// Usually MONO_ENV_OPTIONS looks like:   --profile=jb:prof=timeline,ctl=remote,host=127.0.0.1:55467
		const String prefix = "--profile=";
		if (mono_env_ops.begins_with(prefix)) {
			const String ops = mono_env_ops.substr(prefix.length(), mono_env_ops.length());
			mono_profiler_load(ops.utf8());
		}
	}
}

void gd_mono_debug_init() {
	CharString da_args = OS::get_singleton()->get_environment("GODOT_MONO_DEBUGGER_AGENT").utf8();

	if (da_args.length()) {
		OS::get_singleton()->set_environment("GODOT_MONO_DEBUGGER_AGENT", String());
	}

#ifdef TOOLS_ENABLED
	int da_port = GLOBAL_DEF("mono/debugger_agent/port", 23685);
	bool da_suspend = GLOBAL_DEF("mono/debugger_agent/wait_for_debugger", false);
	int da_timeout = GLOBAL_DEF("mono/debugger_agent/wait_timeout", 3000);

	if (Engine::get_singleton()->is_editor_hint() ||
			ProjectSettings::get_singleton()->get_resource_path().is_empty() ||
			Engine::get_singleton()->is_project_manager_hint()) {
		if (da_args.size() == 0) {
			return;
		}
	}

	if (da_args.length() == 0) {
		da_args = String("--debugger-agent=transport=dt_socket,address=127.0.0.1:" + itos(da_port) +
				",embedding=1,server=y,suspend=" + (da_suspend ? "y,timeout=" + itos(da_timeout) : "n"))
						  .utf8();
	}
#else
	if (da_args.length() == 0) {
		return; // Exported games don't use the project settings to setup the debugger agent
	}
#endif

	// Debugging enabled

	mono_debug_init(MONO_DEBUG_FORMAT_MONO);

	// --debugger-agent=help
	const char *options[] = {
		"--soft-breakpoints",
		da_args.get_data()
	};
	mono_jit_parse_options(2, (char **)options);
}
#endif
} // namespace

namespace {
hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config = nullptr;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
hostfxr_close_fn hostfxr_close = nullptr;

#ifdef _WIN32
static_assert(sizeof(char_t) == sizeof(char16_t));
using HostFxrCharString = Char16String;
#define HOSTFXR_STR(m_str) L##m_str
#else
static_assert(sizeof(char_t) == sizeof(char));
using HostFxrCharString = CharString;
#define HOSTFXR_STR(m_str) m_str
#endif

HostFxrCharString str_to_hostfxr(const String &p_str) {
#ifdef _WIN32
	return p_str.utf16();
#else
	return p_str.utf8();
#endif
}

String str_from_hostfxr(const char_t *p_buffer) {
#ifdef _WIN32
	return String::utf16((const char16_t *)p_buffer);
#else
	return String::utf8((const char *)p_buffer);
#endif
}

const char_t *get_data(const HostFxrCharString &p_char_str) {
	return (const char_t *)p_char_str.get_data();
}

String find_hostfxr() {
	const int HostApiBufferTooSmall = 0x80008098;

	size_t buffer_size = 0;
	int rc = get_hostfxr_path(nullptr, &buffer_size, nullptr);

	if (rc == HostApiBufferTooSmall) {
		// Pre-allocate a large buffer for the path to hostfxr
		Vector<char_t> buffer;
		buffer.resize(buffer_size);

		rc = get_hostfxr_path(buffer.ptrw(), &buffer_size, nullptr);

		if (rc != 0) {
			return String();
		}

		return str_from_hostfxr(buffer.ptr());
	}

	return String();
}

// Forward declarations
bool load_hostfxr() {
	String hostfxr_path = find_hostfxr();

	if (hostfxr_path.is_empty()) {
		return false;
	}

	print_verbose("Found hostfxr: " + hostfxr_path);

	void *lib = nullptr;
	Error err = OS::get_singleton()->open_dynamic_library(hostfxr_path, lib);
	// TODO: Clean up lib handle when shutting down

	if (err != OK) {
		return false;
	}

	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_runtime_config", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_get_runtime_delegate", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_close", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_close = (hostfxr_close_fn)symbol;

	return (hostfxr_initialize_for_runtime_config &&
			hostfxr_get_runtime_delegate &&
			hostfxr_close);
}

load_assembly_and_get_function_pointer_fn initialize_hostfxr(const char_t *p_config_path) {
	hostfxr_handle cxt = nullptr;
	int rc = hostfxr_initialize_for_runtime_config(p_config_path, nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_runtime_config failed");
	}

	void *load_assembly_and_get_function_pointer = nullptr;

	rc = hostfxr_get_runtime_delegate(cxt,
			hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed");
	}

	hostfxr_close(cxt);

	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
} // namespace

static bool _on_core_api_assembly_loaded() {
	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	GDMonoCache::managed_callbacks.Dispatcher_InitializeDefaultGodotTaskScheduler();

#ifdef DEBUG_ENABLED
	// Install the trace listener now before the project assembly is loaded
	GDMonoCache::managed_callbacks.DebuggingUtils_InstallTraceListener();
#endif

	return true;
}

void GDMono::initialize() {
	ERR_FAIL_NULL(Engine::get_singleton());

	print_verbose(".NET: Initializing module...");

	_init_godot_api_hashes();

	if (!load_hostfxr()) {
		ERR_FAIL_MSG(".NET: Failed to load hostfxr");
	}

	auto config_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir()
											  .plus_file("GodotPlugins.runtimeconfig.json"));

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr(get_data(config_path));
	ERR_FAIL_NULL(load_assembly_and_get_function_pointer);

	runtime_initialized = true;

	print_verbose(".NET: hostfxr initialized");

	auto godot_plugins_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir()
													 .plus_file("GodotPlugins.dll"));

	using godot_plugins_initialize_fn = bool (*)(bool, PluginCallbacks *, GDMonoCache::ManagedCallbacks *);
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	int rc = load_assembly_and_get_function_pointer(get_data(godot_plugins_path),
			HOSTFXR_STR("GodotPlugins.Main, GodotPlugins"),
			HOSTFXR_STR("Initialize"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_MSG(rc != 0, ".NET: Failed to get Godot.Plugins Initialize function pointer");

	PluginCallbacks aux_plugin_callbacks;
	GDMonoCache::ManagedCallbacks managed_callbacks;
	bool init_ok = godot_plugins_initialize(Engine::get_singleton()->is_editor_hint(),
			&aux_plugin_callbacks, &managed_callbacks);
	ERR_FAIL_COND_MSG(!init_ok, ".NET: Call to Godot.Plugins Initialize failed");

	GDMonoCache::update_godot_api_cache(managed_callbacks);
	plugin_callbacks = aux_plugin_callbacks;

	print_verbose(".NET: GodotPlugins initialized");

	_on_core_api_assembly_loaded();
}

void GDMono::initialize_load_assemblies() {
#if defined(TOOLS_ENABLED)
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}
#endif

	// Load the project's main assembly. This doesn't necessarily need to succeed.
	// The game may not be using .NET at all, or if the project does use .NET and
	// we're running in the editor, it may just happen to be it wasn't built yet.
	if (!_load_project_assembly()) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			print_error(".NET: Failed to load project assembly");
		}
	}
}

void GDMono::_init_godot_api_hashes() {
#ifdef DEBUG_METHODS_ENABLED
	get_api_core_hash();

#ifdef TOOLS_ENABLED
	get_api_editor_hash();
#endif // TOOLS_ENABLED
#endif // DEBUG_METHODS_ENABLED
}

bool GDMono::_load_project_assembly() {
	String appname = ProjectSettings::get_singleton()->get("application/config/name");
	String appname_safe = OS::get_singleton()->get_safe_dir_name(appname);
	if (appname_safe.is_empty()) {
		appname_safe = "UnnamedProject";
	}

	String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir()
								   .plus_file(appname_safe + ".dll");
	assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);

	return plugin_callbacks.LoadProjectAssemblyCallback(assembly_path.utf16());
}

#warning TODO hot-reload
#if 0
Error GDMono::_unload_scripts_domain() {
	ERR_FAIL_NULL_V(scripts_domain, ERR_BUG);

	CSharpLanguage::get_singleton()->_on_scripts_domain_about_to_unload();

	print_verbose("Mono: Finalizing scripts domain...");

	if (mono_domain_get() != root_domain) {
		mono_domain_set(root_domain, true);
	}

	finalizing_scripts_domain = true;

	if (!mono_domain_finalize(scripts_domain, 2000)) {
		ERR_PRINT("Mono: Domain finalization timeout.");
	}

	finalizing_scripts_domain = false;

	mono_gc_collect(mono_gc_max_generation());

	core_api_assembly = nullptr;
#ifdef TOOLS_ENABLED
	editor_api_assembly = nullptr;
#endif

	project_assembly = nullptr;
#ifdef TOOLS_ENABLED
	tools_assembly = nullptr;
#endif

	MonoDomain *domain = scripts_domain;
	scripts_domain = nullptr;

	print_verbose("Mono: Unloading scripts domain...");

	MonoException *exc = nullptr;
	mono_domain_try_unload(domain, (MonoObject **)&exc);

	if (exc) {
		ERR_PRINT("Exception thrown when unloading scripts domain.");
		GDMonoUtils::debug_unhandled_exception(exc);
		return FAILED;
	}

	return OK;
}

#ifdef GD_MONO_HOT_RELOAD
Error GDMono::reload_scripts_domain() {
	ERR_FAIL_COND_V(!runtime_initialized, ERR_BUG);

	if (scripts_domain) {
		Error domain_unload_err = _unload_scripts_domain();
		ERR_FAIL_COND_V_MSG(domain_unload_err != OK, domain_unload_err, "Mono: Failed to unload scripts domain.");
	}

	Error domain_load_err = _load_scripts_domain();
	ERR_FAIL_COND_V_MSG(domain_load_err != OK, domain_load_err, "Mono: Failed to load scripts domain.");

	// Load assemblies. The API and tools assemblies are required,
	// the application is aborted if these assemblies cannot be loaded.

	if (!_try_load_api_assemblies()) {
		CRASH_NOW_MSG("Failed to load one of the API assemblies.");
	}

#if defined(TOOLS_ENABLED)
	bool tools_assemblies_loaded = _load_tools_assemblies();
	CRASH_COND_MSG(!tools_assemblies_loaded, "Mono: Failed to load '" TOOLS_ASM_NAME "' assemblies.");
#endif

	// Load the project's main assembly. Here, during hot-reloading, we do
	// consider failing to load the project's main assembly to be an error.
	// However, unlike the API and tools assemblies, the application can continue working.
	if (!_load_project_assembly()) {
		print_error("Mono: Failed to load project assembly");
		return ERR_CANT_OPEN;
	}

	return OK;
}
#endif
#endif

#warning TODO Reimplement in C#
#if 0
void GDMono::unhandled_exception_hook(MonoObject *p_exc, void *) {
	// This method will be called by the runtime when a thrown exception is not handled.
	// It won't be called when we manually treat a thrown exception as unhandled.
	// We assume the exception was already printed before calling this hook.

#ifdef DEBUG_ENABLED
	GDMonoUtils::debug_send_unhandled_exception_error((MonoException *)p_exc);
	if (EngineDebugger::is_active()) {
		EngineDebugger::get_singleton()->poll_events(false);
	}
#endif

	exit(mono_environment_exitcode_get());

	GD_UNREACHABLE();
}
#endif

GDMono::GDMono() {
	singleton = this;

	runtime_initialized = false;
	finalizing_scripts_domain = false;

	api_core_hash = 0;
#ifdef TOOLS_ENABLED
	api_editor_hash = 0;
#endif
}

GDMono::~GDMono() {
	if (is_runtime_initialized()) {
#warning "TODO assembly unloading for cleanup of disposables (including managed RefCounteds)"
#if 0
		if (scripts_domain) {
			Error err = _unload_scripts_domain();
			if (err != OK) {
				ERR_PRINT("Mono: Failed to unload scripts domain.");
			}
		}

		print_verbose("Mono: Runtime cleanup...");

		mono_jit_cleanup(root_domain);

		print_verbose("Mono: Finalized");
#endif

		runtime_initialized = false;
	}

#if defined(ANDROID_ENABLED)
	gdmono::android::support::cleanup();
#endif

	singleton = nullptr;
}

namespace mono_bind {

GodotSharp *GodotSharp::singleton = nullptr;

bool GodotSharp::_is_runtime_initialized() {
	return GDMono::get_singleton() != nullptr && GDMono::get_singleton()->is_runtime_initialized();
}

void GodotSharp::_reload_assemblies(bool p_soft_reload) {
#ifdef GD_MONO_HOT_RELOAD
	CRASH_COND(CSharpLanguage::get_singleton() == nullptr);
	// This method may be called more than once with `call_deferred`, so we need to check
	// again if reloading is needed to avoid reloading multiple times unnecessarily.
	if (CSharpLanguage::get_singleton()->is_assembly_reloading_needed()) {
		CSharpLanguage::get_singleton()->reload_assemblies(p_soft_reload);
	}
#endif
}

void GodotSharp::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_runtime_initialized"), &GodotSharp::_is_runtime_initialized);
	ClassDB::bind_method(D_METHOD("_reload_assemblies"), &GodotSharp::_reload_assemblies);
}

GodotSharp::GodotSharp() {
	singleton = this;
}

GodotSharp::~GodotSharp() {
	singleton = nullptr;
}

} // namespace mono_bind
