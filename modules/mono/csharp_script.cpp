/*************************************************************************/
/*  csharp_script.cpp                                                    */
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

#include "csharp_script.h"

#include <stdint.h>

#include "core/config/project_settings.h"
#include "core/debugger/engine_debugger.h"
#include "core/debugger/script_debugger.h"
#include "core/io/file_access.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"

#ifdef TOOLS_ENABLED
#include "core/os/keyboard.h"
#include "editor/bindings_generator.h"
#include "editor/editor_internal_calls.h"
#include "editor/editor_node.h"
#include "editor/editor_settings.h"
#include "editor/node_dock.h"
#include "editor/script_templates/templates.gen.h"
#endif

#ifdef DEBUG_METHODS_ENABLED
#include "class_db_api_json.h"
#endif

#include "godotsharp_dirs.h"
#include "managed_callable.h"
#include "mono_gd/gd_mono_cache.h"
#include "signal_awaiter_utils.h"
#include "utils/macros.h"
#include "utils/string_utils.h"

#define CACHED_STRING_NAME(m_var) (CSharpLanguage::get_singleton()->get_string_names().m_var)

#ifdef TOOLS_ENABLED
static bool _create_project_solution_if_needed() {
	String sln_path = GodotSharpDirs::get_project_sln_path();
	String csproj_path = GodotSharpDirs::get_project_csproj_path();

	if (!FileAccess::exists(sln_path) || !FileAccess::exists(csproj_path)) {
		// A solution does not yet exist, create a new one

		CRASH_COND(CSharpLanguage::get_singleton()->get_godotsharp_editor() == nullptr);
		return CSharpLanguage::get_singleton()->get_godotsharp_editor()->call("CreateProjectSolution");
	}

	return true;
}
#endif

CSharpLanguage *CSharpLanguage::singleton = nullptr;

GDNativeInstanceBindingCallbacks CSharpLanguage::_instance_binding_callbacks = {
	&_instance_binding_create_callback,
	&_instance_binding_free_callback,
	&_instance_binding_reference_callback
};

String CSharpLanguage::get_name() const {
	return "C#";
}

String CSharpLanguage::get_type() const {
	return "CSharpScript";
}

String CSharpLanguage::get_extension() const {
	return "cs";
}

Error CSharpLanguage::execute_file(const String &p_path) {
	// ??
	return OK;
}

extern void *godotsharp_pinvoke_funcs[176];
[[maybe_unused]] volatile void **do_not_strip_godotsharp_pinvoke_funcs;
#ifdef TOOLS_ENABLED
extern void *godotsharp_editor_pinvoke_funcs[32];
[[maybe_unused]] volatile void **do_not_strip_godotsharp_editor_pinvoke_funcs;
#endif

void CSharpLanguage::init() {
#ifdef DEBUG_METHODS_ENABLED
	if (OS::get_singleton()->get_cmdline_args().find("--class-db-json")) {
		class_db_api_to_json("user://class_db_api.json", ClassDB::API_CORE);
#ifdef TOOLS_ENABLED
		class_db_api_to_json("user://class_db_api_editor.json", ClassDB::API_EDITOR);
#endif
	}
#endif

	// Hopefully this will be enough for all compilers. Otherwise we could use the printf on fake getenv trick.
	do_not_strip_godotsharp_pinvoke_funcs = (volatile void **)godotsharp_pinvoke_funcs;
#ifdef TOOLS_ENABLED
	do_not_strip_godotsharp_editor_pinvoke_funcs = (volatile void **)godotsharp_editor_pinvoke_funcs;
#endif

#if defined(TOOLS_ENABLED) && defined(DEBUG_METHODS_ENABLED)
	// Generate the bindings here, before loading assemblies. The Godot assemblies
	// may be missing if the glue wasn't generated yet in order to build them.
	List<String> cmdline_args = OS::get_singleton()->get_cmdline_args();
	BindingsGenerator::handle_cmdline_args(cmdline_args);
#endif

	gdmono = memnew(GDMono);
	gdmono->initialize();

	if (gdmono->is_runtime_initialized()) {
		gdmono->initialize_load_assemblies();
	}

#ifdef TOOLS_ENABLED
	EditorNode::add_init_callback(&_editor_init_callback);
#endif
}

void CSharpLanguage::finish() {
	finalize();
}

void CSharpLanguage::finalize() {
	if (finalized) {
		return;
	}

	finalizing = true;

	// Make sure all script binding gchandles are released before finalizing GDMono
	for (KeyValue<Object *, CSharpScriptBinding> &E : script_bindings) {
		CSharpScriptBinding &script_binding = E.value;

		if (!script_binding.gchandle.is_released()) {
			script_binding.gchandle.release();
			script_binding.inited = false;
		}
	}

	if (gdmono) {
		memdelete(gdmono);
		gdmono = nullptr;
	}

	// Clear here, after finalizing all domains to make sure there is nothing else referencing the elements.
	script_bindings.clear();

#ifdef DEBUG_ENABLED
	for (const KeyValue<ObjectID, int> &E : unsafe_object_references) {
		const ObjectID &id = E.key;
		Object *obj = ObjectDB::get_instance(id);

		if (obj) {
			ERR_PRINT("Leaked unsafe reference to object: " + obj->to_string());
		} else {
			ERR_PRINT("Leaked unsafe reference to deleted object: " + itos(id));
		}
	}
#endif

	memdelete(managed_callable_middleman);

	finalizing = false;
	finalized = true;
}

void CSharpLanguage::get_reserved_words(List<String> *p_words) const {
	static const char *_reserved_words[] = {
		// Reserved keywords
		"abstract",
		"as",
		"base",
		"bool",
		"break",
		"byte",
		"case",
		"catch",
		"char",
		"checked",
		"class",
		"const",
		"continue",
		"decimal",
		"default",
		"delegate",
		"do",
		"double",
		"else",
		"enum",
		"event",
		"explicit",
		"extern",
		"false",
		"finally",
		"fixed",
		"float",
		"for",
		"foreach",
		"goto",
		"if",
		"implicit",
		"in",
		"int",
		"interface",
		"internal",
		"is",
		"lock",
		"long",
		"namespace",
		"new",
		"null",
		"object",
		"operator",
		"out",
		"override",
		"params",
		"private",
		"protected",
		"public",
		"readonly",
		"ref",
		"return",
		"sbyte",
		"sealed",
		"short",
		"sizeof",
		"stackalloc",
		"static",
		"string",
		"struct",
		"switch",
		"this",
		"throw",
		"true",
		"try",
		"typeof",
		"uint",
		"ulong",
		"unchecked",
		"unsafe",
		"ushort",
		"using",
		"virtual",
		"void",
		"volatile",
		"while",

		// Contextual keywords. Not reserved words, but I guess we should include
		// them because this seems to be used only for syntax highlighting.
		"add",
		"alias",
		"ascending",
		"async",
		"await",
		"by",
		"descending",
		"dynamic",
		"equals",
		"from",
		"get",
		"global",
		"group",
		"into",
		"join",
		"let",
		"nameof",
		"on",
		"orderby",
		"partial",
		"remove",
		"select",
		"set",
		"value",
		"var",
		"when",
		"where",
		"yield",
		nullptr
	};

	const char **w = _reserved_words;

	while (*w) {
		p_words->push_back(*w);
		w++;
	}
}

bool CSharpLanguage::is_control_flow_keyword(String p_keyword) const {
	return p_keyword == "break" ||
			p_keyword == "case" ||
			p_keyword == "catch" ||
			p_keyword == "continue" ||
			p_keyword == "default" ||
			p_keyword == "do" ||
			p_keyword == "else" ||
			p_keyword == "finally" ||
			p_keyword == "for" ||
			p_keyword == "foreach" ||
			p_keyword == "goto" ||
			p_keyword == "if" ||
			p_keyword == "return" ||
			p_keyword == "switch" ||
			p_keyword == "throw" ||
			p_keyword == "try" ||
			p_keyword == "while";
}

void CSharpLanguage::get_comment_delimiters(List<String> *p_delimiters) const {
	p_delimiters->push_back("//"); // single-line comment
	p_delimiters->push_back("/* */"); // delimited comment
}

void CSharpLanguage::get_string_delimiters(List<String> *p_delimiters) const {
	p_delimiters->push_back("' '"); // character literal
	p_delimiters->push_back("\" \""); // regular string literal
	p_delimiters->push_back("@\" \""); // verbatim string literal
	// Generic string highlighting suffices as a workaround for now.
}

static String get_base_class_name(const String &p_base_class_name, const String p_class_name) {
	String base_class = p_base_class_name;
	if (p_class_name == base_class) {
		base_class = "Godot." + base_class;
	}
	return base_class;
}

bool CSharpLanguage::is_using_templates() {
	return true;
}

Ref<Script> CSharpLanguage::make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<CSharpScript> script;
	script.instantiate();

	String class_name_no_spaces = p_class_name.replace(" ", "_");
	String base_class_name = get_base_class_name(p_base_class_name, class_name_no_spaces);
	String processed_template = p_template;
	processed_template = processed_template.replace("_BINDINGS_NAMESPACE_", BINDINGS_NAMESPACE)
								 .replace("_BASE_", base_class_name)
								 .replace("_CLASS_", class_name_no_spaces)
								 .replace("_TS_", _get_indentation());
	script->set_source_code(processed_template);
	return script;
}

Vector<ScriptLanguage::ScriptTemplate> CSharpLanguage::get_built_in_templates(StringName p_object) {
	Vector<ScriptLanguage::ScriptTemplate> templates;
#ifdef TOOLS_ENABLED
	for (int i = 0; i < TEMPLATES_ARRAY_SIZE; i++) {
		if (TEMPLATES[i].inherit == p_object) {
			templates.append(TEMPLATES[i]);
		}
	}
#endif
	return templates;
}

String CSharpLanguage::validate_path(const String &p_path) const {
	String class_name = p_path.get_file().get_basename();
	List<String> keywords;
	get_reserved_words(&keywords);
	if (keywords.find(class_name)) {
		return RTR("Class name can't be a reserved keyword");
	}
	return "";
}

Script *CSharpLanguage::create_script() const {
	return memnew(CSharpScript);
}

bool CSharpLanguage::has_named_classes() const {
	return false;
}

bool CSharpLanguage::supports_builtin_mode() const {
	return false;
}

#ifdef TOOLS_ENABLED
static String variant_type_to_managed_name(const String &p_var_type_name) {
	if (p_var_type_name.is_empty()) {
		return "object";
	}

	if (!ClassDB::class_exists(p_var_type_name)) {
		return p_var_type_name;
	}

	if (p_var_type_name == Variant::get_type_name(Variant::OBJECT)) {
		return "Godot.Object";
	}

	if (p_var_type_name == Variant::get_type_name(Variant::FLOAT)) {
#ifdef REAL_T_IS_DOUBLE
		return "double";
#else
		return "float";
#endif
	}

	if (p_var_type_name == Variant::get_type_name(Variant::STRING)) {
		return "string"; // I prefer this one >:[
	}

	if (p_var_type_name == Variant::get_type_name(Variant::DICTIONARY)) {
		return "Collections.Dictionary";
	}

	if (p_var_type_name == Variant::get_type_name(Variant::ARRAY)) {
		return "Collections.Array";
	}

	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_BYTE_ARRAY)) {
		return "byte[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_INT32_ARRAY)) {
		return "int[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_INT64_ARRAY)) {
		return "long[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_FLOAT32_ARRAY)) {
		return "float[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_FLOAT64_ARRAY)) {
		return "double[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_STRING_ARRAY)) {
		return "string[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_VECTOR2_ARRAY)) {
		return "Vector2[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_VECTOR3_ARRAY)) {
		return "Vector3[]";
	}
	if (p_var_type_name == Variant::get_type_name(Variant::PACKED_COLOR_ARRAY)) {
		return "Color[]";
	}

	if (p_var_type_name == Variant::get_type_name(Variant::SIGNAL)) {
		return "SignalInfo";
	}

	Variant::Type var_types[] = {
		Variant::BOOL,
		Variant::INT,
		Variant::VECTOR2,
		Variant::VECTOR2I,
		Variant::RECT2,
		Variant::RECT2I,
		Variant::VECTOR3,
		Variant::VECTOR3I,
		Variant::TRANSFORM2D,
		Variant::VECTOR4,
		Variant::VECTOR4I,
		Variant::PLANE,
		Variant::QUATERNION,
		Variant::AABB,
		Variant::BASIS,
		Variant::TRANSFORM3D,
		Variant::PROJECTION,
		Variant::COLOR,
		Variant::STRING_NAME,
		Variant::NODE_PATH,
		Variant::RID,
		Variant::CALLABLE
	};

	for (unsigned int i = 0; i < sizeof(var_types) / sizeof(Variant::Type); i++) {
		if (p_var_type_name == Variant::get_type_name(var_types[i])) {
			return p_var_type_name;
		}
	}

	return "object";
}

String CSharpLanguage::make_function(const String &, const String &p_name, const PackedStringArray &p_args) const {
	// FIXME
	// - Due to Godot's API limitation this just appends the function to the end of the file
	// - Use fully qualified name if there is ambiguity
	String s = "private void " + p_name + "(";
	for (int i = 0; i < p_args.size(); i++) {
		const String &arg = p_args[i];

		if (i > 0) {
			s += ", ";
		}

		s += variant_type_to_managed_name(arg.get_slice(":", 1)) + " " + escape_csharp_keyword(arg.get_slice(":", 0));
	}
	s += ")\n{\n    // Replace with function body.\n}\n";

	return s;
}
#else
String CSharpLanguage::make_function(const String &, const String &, const PackedStringArray &) const {
	return String();
}
#endif

String CSharpLanguage::_get_indentation() const {
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		bool use_space_indentation = EDITOR_GET("text_editor/behavior/indent/type");

		if (use_space_indentation) {
			int indent_size = EDITOR_GET("text_editor/behavior/indent/size");

			String space_indent = "";
			for (int i = 0; i < indent_size; i++) {
				space_indent += " ";
			}
			return space_indent;
		}
	}
#endif
	return "\t";
}

String CSharpLanguage::debug_get_error() const {
	return _debug_error;
}

int CSharpLanguage::debug_get_stack_level_count() const {
	if (_debug_parse_err_line >= 0) {
		return 1;
	}

	// TODO: StackTrace
	return 1;
}

int CSharpLanguage::debug_get_stack_level_line(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return _debug_parse_err_line;
	}

	// TODO: StackTrace
	return 1;
}

String CSharpLanguage::debug_get_stack_level_function(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return String();
	}

	// TODO: StackTrace
	return String();
}

String CSharpLanguage::debug_get_stack_level_source(int p_level) const {
	if (_debug_parse_err_line >= 0) {
		return _debug_parse_err_file;
	}

	// TODO: StackTrace
	return String();
}

#warning TODO
#if 0
Vector<ScriptLanguage::StackInfo> CSharpLanguage::debug_get_current_stack_info() {
#ifdef DEBUG_ENABLED
	// Printing an error here will result in endless recursion, so we must be careful
	static thread_local bool _recursion_flag_ = false;
	if (_recursion_flag_) {
		return Vector<StackInfo>();
	}
	_recursion_flag_ = true;
	SCOPE_EXIT {
		_recursion_flag_ = false;
	};

	GD_MONO_SCOPE_THREAD_ATTACH;

	if (!gdmono->is_runtime_initialized() || !GDMono::get_singleton()->get_core_api_assembly() || !GDMonoCache::cached_data.corlib_cache_updated) {
		return Vector<StackInfo>();
	}

	MonoObject *stack_trace = mono_object_new(mono_domain_get(), CACHED_CLASS(System_Diagnostics_StackTrace)->get_mono_ptr());

	MonoBoolean need_file_info = true;
	void *ctor_args[1] = { &need_file_info };

	CACHED_METHOD(System_Diagnostics_StackTrace, ctor_bool)->invoke_raw(stack_trace, ctor_args);

	Vector<StackInfo> si;
	si = stack_trace_get_info(stack_trace);

	return si;
#else
	return Vector<StackInfo>();
#endif
}

#ifdef DEBUG_ENABLED
Vector<ScriptLanguage::StackInfo> CSharpLanguage::stack_trace_get_info(MonoObject *p_stack_trace) {
	// Printing an error here will result in endless recursion, so we must be careful
	static thread_local bool _recursion_flag_ = false;
	if (_recursion_flag_) {
		return Vector<StackInfo>();
	}
	_recursion_flag_ = true;
	SCOPE_EXIT {
		_recursion_flag_ = false;
	};

	GD_MONO_SCOPE_THREAD_ATTACH;

	MonoException *exc = nullptr;

	MonoArray *frames = CACHED_METHOD_THUNK(System_Diagnostics_StackTrace, GetFrames).invoke(p_stack_trace, &exc);

	if (exc) {
		GDMonoUtils::debug_print_unhandled_exception(exc);
		return Vector<StackInfo>();
	}

	int frame_count = mono_array_length(frames);

	if (frame_count <= 0) {
		return Vector<StackInfo>();
	}

	Vector<StackInfo> si;
	si.resize(frame_count);

	for (int i = 0; i < frame_count; i++) {
		StackInfo &sif = si.write[i];
		MonoObject *frame = mono_array_get(frames, MonoObject *, i);

		MonoString *file_name;
		int file_line_num;
		MonoString *method_decl;
		CACHED_METHOD_THUNK(DebuggingUtils, GetStackFrameInfo).invoke(frame, &file_name, &file_line_num, &method_decl, &exc);

		if (exc) {
			GDMonoUtils::debug_print_unhandled_exception(exc);
			return Vector<StackInfo>();
		}

		// TODO
		// what if the StackFrame method is null (method_decl is empty). should we skip this frame?
		// can reproduce with a MissingMethodException on internal calls

		sif.file = GDMonoMarshal::mono_string_to_godot(file_name);
		sif.line = file_line_num;
		sif.func = GDMonoMarshal::mono_string_to_godot(method_decl);
	}

	return si;
}
#endif
#else
Vector<ScriptLanguage::StackInfo> CSharpLanguage::debug_get_current_stack_info() {
	return Vector<StackInfo>();
}
#endif

void CSharpLanguage::post_unsafe_reference(Object *p_obj) {
#ifdef DEBUG_ENABLED
	MutexLock lock(unsafe_object_references_lock);
	ObjectID id = p_obj->get_instance_id();
	unsafe_object_references[id]++;
#endif
}

void CSharpLanguage::pre_unsafe_unreference(Object *p_obj) {
#ifdef DEBUG_ENABLED
	MutexLock lock(unsafe_object_references_lock);
	ObjectID id = p_obj->get_instance_id();
	HashMap<ObjectID, int>::Iterator elem = unsafe_object_references.find(id);
	ERR_FAIL_NULL(elem);
	if (--elem->value == 0) {
		unsafe_object_references.remove(elem);
	}
#endif
}

void CSharpLanguage::frame() {
	if (gdmono && gdmono->is_runtime_initialized() && GDMonoCache::godot_api_cache_updated) {
		GDMonoCache::managed_callbacks.ScriptManagerBridge_FrameCallback();
	}
}

void CSharpLanguage::reload_all_scripts() {
#ifdef GD_MONO_HOT_RELOAD
	if (is_assembly_reloading_needed()) {
		reload_assemblies(false);
	}
#endif
}

void CSharpLanguage::reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	(void)p_script; // UNUSED

	CRASH_COND(!Engine::get_singleton()->is_editor_hint());

#ifdef TOOLS_ENABLED
	get_godotsharp_editor()->get_node(NodePath("HotReloadAssemblyWatcher"))->call("RestartTimer");
#endif

#ifdef GD_MONO_HOT_RELOAD
	if (is_assembly_reloading_needed()) {
		reload_assemblies(p_soft_reload);
	}
#endif
}

#ifdef GD_MONO_HOT_RELOAD
bool CSharpLanguage::is_assembly_reloading_needed() {
	if (!gdmono->is_runtime_initialized()) {
		return false;
	}

#warning TODO
#if 0
	GDMonoAssembly *proj_assembly = gdmono->get_project_assembly();

	String appname_safe = ProjectSettings::get_singleton()->get_safe_project_name();

	appname_safe += ".dll";

	if (proj_assembly) {
		String proj_asm_path = proj_assembly->get_path();

		if (!FileAccess::exists(proj_asm_path)) {
			// Maybe it wasn't loaded from the default path, so check this as well
			proj_asm_path = GodotSharpDirs::get_res_temp_assemblies_dir().plus_file(appname_safe);
			if (!FileAccess::exists(proj_asm_path)) {
				return false; // No assembly to load
			}
		}

		if (FileAccess::get_modified_time(proj_asm_path) <= proj_assembly->get_modified_time()) {
			return false; // Already up to date
		}
	} else {
		if (!FileAccess::exists(GodotSharpDirs::get_res_temp_assemblies_dir().plus_file(appname_safe))) {
			return false; // No assembly to load
		}
	}

	return true;
#else
	return false;
#endif
}

void CSharpLanguage::reload_assemblies(bool p_soft_reload) {
	if (!gdmono->is_runtime_initialized()) {
		return;
	}

#warning TODO ALCs after switching to .NET 6
#if 0
	// There is no soft reloading with Mono. It's always hard reloading.

	List<Ref<CSharpScript>> scripts;

	{
		MutexLock lock(script_instances_mutex);

		for (SelfList<CSharpScript> *elem = script_list.first(); elem; elem = elem->next()) {
			// Cast to CSharpScript to avoid being erased by accident
			scripts.push_back(Ref<CSharpScript>(elem->self()));
		}
	}

	scripts.sort_custom<CSharpScriptDepSort>(); // Update in inheritance dependency order

	// Serialize managed callables
	{
		MutexLock lock(ManagedCallable::instances_mutex);

		for (SelfList<ManagedCallable> *elem = ManagedCallable::instances.first(); elem; elem = elem->next()) {
			ManagedCallable *managed_callable = elem->self();

			Array serialized_data;
			MonoObject *managed_serialized_data = GDMonoMarshal::variant_to_mono_object(serialized_data);

			MonoException *exc = nullptr;
			bool success = (bool)GDMonoCache::managed_callbacks.methodthunk_DelegateUtils_TrySerializeDelegateWithGCHandle
								   .invoke(managed_callable->delegate_handle,
										   managed_serialized_data, &exc);

			if (exc) {
				GDMonoUtils::debug_print_unhandled_exception(exc);
				continue;
			}

			if (success) {
				ManagedCallable::instances_pending_reload.insert(managed_callable, serialized_data);
			} else if (OS::get_singleton()->is_stdout_verbose()) {
				OS::get_singleton()->print("Failed to serialize delegate\n");
			}
		}
	}

	List<Ref<CSharpScript>> to_reload;

	// We need to keep reference instances alive during reloading
	List<Ref<RefCounted>> rc_instances;

	for (const KeyValue<Object *, CSharpScriptBinding> &E : script_bindings) {
		const CSharpScriptBinding &script_binding = E.value;
		RefCounted *rc = Object::cast_to<RefCounted>(script_binding.owner);
		if (rc) {
			rc_instances.push_back(Ref<RefCounted>(rc));
		}
	}

	// As scripts are going to be reloaded, must proceed without locking here

	for (Ref<CSharpScript> &script : scripts) {
		// If someone removes a script from a node, deletes the script, builds, adds a script to the
		// same node, then builds again, the script might have no path and also no script_class. In
		// that case, we can't (and don't need to) reload it.
		if (script->get_path().is_empty() && !script->script_class) {
			continue;
		}

		to_reload.push_back(script);

		if (script->get_path().is_empty()) {
			script->tied_class_name_for_reload = script->script_class->get_name_for_lookup();
			script->tied_class_namespace_for_reload = script->script_class->get_namespace();
		}

		// Script::instances are deleted during managed object disposal, which happens on domain finalize.
		// Only placeholders are kept. Therefore we need to keep a copy before that happens.

		for (Object *obj : script->instances) {
			script->pending_reload_instances.insert(obj->get_instance_id());

			RefCounted *rc = Object::cast_to<RefCounted>(obj);
			if (rc) {
				rc_instances.push_back(Ref<RefCounted>(rc));
			}
		}

#ifdef TOOLS_ENABLED
		for (PlaceHolderScriptInstance *script_instance : script->placeholders) {
			Object *obj = script_instance->get_owner();
			script->pending_reload_instances.insert(obj->get_instance_id());

			RefCounted *rc = Object::cast_to<RefCounted>(obj);
			if (rc) {
				rc_instances.push_back(Ref<RefCounted>(rc));
			}
		}
#endif

		// Save state and remove script from instances
		RBMap<ObjectID, CSharpScript::StateBackup> &owners_map = script->pending_reload_state;

		for (Object *obj : script->instances) {
			ERR_CONTINUE(!obj->get_script_instance());

			CSharpInstance *csi = static_cast<CSharpInstance *>(obj->get_script_instance());

			// Call OnBeforeSerialize
			if (csi->script->script_class->implements_interface(GDMonoCache::cached_data.class_ISerializationListener)) {
				obj->get_script_instance()->call(string_names.on_before_serialize);
			}

			// Save instance info
			CSharpScript::StateBackup state;

			// TODO: Proper state backup (Not only variants, serialize managed state of scripts)
			csi->get_properties_state_for_reloading(state.properties);
			csi->get_event_signals_state_for_reloading(state.event_signals);

			owners_map[obj->get_instance_id()] = state;
		}
	}

	// After the state of all instances is saved, clear scripts and script instances
	for (Ref<CSharpScript> &script : scripts) {
		while (script->instances.begin()) {
			Object *obj = *script->instances.begin();
			obj->set_script(Ref<RefCounted>()); // Remove script and existing script instances (placeholder are not removed before domain reload)
		}

		script->_clear();
	}

	// Do domain reload
	if (gdmono->reload_scripts_domain() != OK) {
		// Failed to reload the scripts domain
		// Make sure to add the scripts back to their owners before returning
		for (Ref<CSharpScript> &scr : to_reload) {
			for (const KeyValue<ObjectID, CSharpScript::StateBackup> &F : scr->pending_reload_state) {
				Object *obj = ObjectDB::get_instance(F.key);

				if (!obj) {
					continue;
				}

				ObjectID obj_id = obj->get_instance_id();

				// Use a placeholder for now to avoid losing the state when saving a scene

				PlaceHolderScriptInstance *placeholder = scr->placeholder_instance_create(obj);
				obj->set_script_instance(placeholder);

#ifdef TOOLS_ENABLED
				// Even though build didn't fail, this tells the placeholder to keep properties and
				// it allows using property_set_fallback for restoring the state without a valid script.
				scr->placeholder_fallback_enabled = true;
#endif

				// Restore Variant properties state, it will be kept by the placeholder until the next script reloading
				for (const Pair<StringName, Variant> &G : scr->pending_reload_state[obj_id].properties) {
					placeholder->property_set_fallback(G.first, G.second, nullptr);
				}

				scr->pending_reload_state.erase(obj_id);
			}
		}

		return;
	}

	List<Ref<CSharpScript>> to_reload_state;

	for (Ref<CSharpScript> &script : to_reload) {
#ifdef TOOLS_ENABLED
		script->exports_invalidated = true;
#endif

		if (!script->get_path().is_empty()) {
			script->reload(p_soft_reload);

			if (!script->valid) {
				script->pending_reload_instances.clear();
				continue;
			}
		} else {
			const StringName &class_namespace = script->tied_class_namespace_for_reload;
			const StringName &class_name = script->tied_class_name_for_reload;
			GDMonoAssembly *project_assembly = gdmono->get_project_assembly();

			// Search in project and tools assemblies first as those are the most likely to have the class
			GDMonoClass *script_class = (project_assembly ? project_assembly->get_class(class_namespace, class_name) : nullptr);

#ifdef TOOLS_ENABLED
			if (!script_class) {
				GDMonoAssembly *tools_assembly = gdmono->get_tools_assembly();
				script_class = (tools_assembly ? tools_assembly->get_class(class_namespace, class_name) : nullptr);
			}
#endif

			if (!script_class) {
				script_class = gdmono->get_class(class_namespace, class_name);
			}

			if (!script_class) {
				// The class was removed, can't reload
				script->pending_reload_instances.clear();
				continue;
			}

			bool obj_type = GDMonoCache::cached_data.class_GodotObject->is_assignable_from(script_class);
			if (!obj_type) {
				// The class no longer inherits Godot.Object, can't reload
				script->pending_reload_instances.clear();
				continue;
			}

			GDMonoClass *native = GDMonoUtils::get_class_native_base(script_class);

			CSharpScript::initialize_for_managed_type(script, script_class, native);
		}

		StringName native_name = NATIVE_GDMONOCLASS_NAME(script->native);

		{
			for (const ObjectID &obj_id : script->pending_reload_instances) {
				Object *obj = ObjectDB::get_instance(obj_id);

				if (!obj) {
					script->pending_reload_state.erase(obj_id);
					continue;
				}

				if (!ClassDB::is_parent_class(obj->get_class_name(), native_name)) {
					// No longer inherits the same compatible type, can't reload
					script->pending_reload_state.erase(obj_id);
					continue;
				}

				ScriptInstance *si = obj->get_script_instance();

#ifdef TOOLS_ENABLED
				if (si) {
					// If the script instance is not null, then it must be a placeholder.
					// Non-placeholder script instances are removed in godot_icall_Object_Disposed.
					CRASH_COND(!si->is_placeholder());

					if (script->is_tool() || ScriptServer::is_scripting_enabled()) {
						// Replace placeholder with a script instance

						CSharpScript::StateBackup &state_backup = script->pending_reload_state[obj_id];

						// Backup placeholder script instance state before replacing it with a script instance
						si->get_property_state(state_backup.properties);

						ScriptInstance *script_instance = script->instance_create(obj);

						if (script_instance) {
							script->placeholders.erase(static_cast<PlaceHolderScriptInstance *>(si));
							obj->set_script_instance(script_instance);
						}
					}

					continue;
				}
#else
				CRASH_COND(si != nullptr);
#endif
				// Re-create script instance
				obj->set_script(script); // will create the script instance as well
			}
		}

		to_reload_state.push_back(script);
	}

	for (Ref<CSharpScript> &script : to_reload_state) {
		for (const ObjectID &obj_id : script->pending_reload_instances) {
			Object *obj = ObjectDB::get_instance(obj_id);

			if (!obj) {
				script->pending_reload_state.erase(obj_id);
				continue;
			}

			ERR_CONTINUE(!obj->get_script_instance());

			// TODO: Restore serialized state

			CSharpScript::StateBackup &state_backup = script->pending_reload_state[obj_id];

			for (const Pair<StringName, Variant> &G : state_backup.properties) {
				obj->get_script_instance()->set(G.first, G.second);
			}

			CSharpInstance *csi = CAST_CSHARP_INSTANCE(obj->get_script_instance());

			if (csi) {
				for (const Pair<StringName, Array> &G : state_backup.event_signals) {
					const StringName &name = G.first;
					const Array &serialized_data = G.second;

					HashMap<StringName, GDMonoField *>::Iterator match = script->event_signals.find(name);

					if (!match) {
						// The event or its signal attribute were removed
						continue;
					}

					GDMonoField *event_signal_field = match->value;

					MonoObject *managed_serialized_data = GDMonoMarshal::variant_to_mono_object(serialized_data);
					MonoDelegate *delegate = nullptr;

					MonoException *exc = nullptr;
					bool success = (bool)GDMonoCache::managed_callbacks.methodthunk_DelegateUtils_TryDeserializeDelegate.invoke(managed_serialized_data, &delegate, &exc);

					if (exc) {
						GDMonoUtils::debug_print_unhandled_exception(exc);
						continue;
					}

					if (success) {
						ERR_CONTINUE(delegate == nullptr);
						event_signal_field->set_value(csi->get_mono_object(), (MonoObject *)delegate);
					} else if (OS::get_singleton()->is_stdout_verbose()) {
						OS::get_singleton()->print("Failed to deserialize event signal delegate\n");
					}
				}

				// Call OnAfterDeserialization
				if (csi->script->script_class->implements_interface(GDMonoCache::cached_data.class_ISerializationListener)) {
					obj->get_script_instance()->call(string_names.on_after_deserialize);
				}
			}
		}

		script->pending_reload_instances.clear();
	}

	// Deserialize managed callables
	{
		MutexLock lock(ManagedCallable::instances_mutex);

		for (const KeyValue<ManagedCallable *, Array> &elem : ManagedCallable::instances_pending_reload) {
			ManagedCallable *managed_callable = elem.key;
			const Array &serialized_data = elem.value;

			MonoObject *managed_serialized_data = GDMonoMarshal::variant_to_mono_object(serialized_data);
			void *delegate = nullptr;

			MonoException *exc = nullptr;
			bool success = (bool)GDMonoCache::managed_callbacks.methodthunk_DelegateUtils_TryDeserializeDelegateWithGCHandle
								   .invoke(managed_serialized_data, &delegate, &exc);

			if (exc) {
				GDMonoUtils::debug_print_unhandled_exception(exc);
				continue;
			}

			if (success) {
				ERR_CONTINUE(delegate == nullptr);
				managed_callable->delegate_handle = delegate;
			} else if (OS::get_singleton()->is_stdout_verbose()) {
				OS::get_singleton()->print("Failed to deserialize delegate\n");
			}
		}

		ManagedCallable::instances_pending_reload.clear();
	}

#ifdef TOOLS_ENABLED
	// FIXME: Hack to refresh editor in order to display new properties and signals. See if there is a better alternative.
	if (Engine::get_singleton()->is_editor_hint()) {
		InspectorDock::get_inspector_singleton()->update_tree();
		NodeDock::get_singleton()->update_lists();
	}
#endif
#endif
}
#endif

void CSharpLanguage::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("cs");
}

#ifdef TOOLS_ENABLED
Error CSharpLanguage::open_in_external_editor(const Ref<Script> &p_script, int p_line, int p_col) {
	return (Error)(int)get_godotsharp_editor()->call("OpenInExternalEditor", p_script, p_line, p_col);
}

bool CSharpLanguage::overrides_external_editor() {
	return get_godotsharp_editor()->call("OverridesExternalEditor");
}
#endif

bool CSharpLanguage::debug_break_parse(const String &p_file, int p_line, const String &p_error) {
	// Not a parser error in our case, but it's still used for other type of errors
	if (EngineDebugger::is_active() && Thread::get_caller_id() == Thread::get_main_id()) {
		_debug_parse_err_line = p_line;
		_debug_parse_err_file = p_file;
		_debug_error = p_error;
		EngineDebugger::get_script_debugger()->debug(this, false, true);
		return true;
	} else {
		return false;
	}
}

bool CSharpLanguage::debug_break(const String &p_error, bool p_allow_continue) {
	if (EngineDebugger::is_active() && Thread::get_caller_id() == Thread::get_main_id()) {
		_debug_parse_err_line = -1;
		_debug_parse_err_file = "";
		_debug_error = p_error;
		EngineDebugger::get_script_debugger()->debug(this, p_allow_continue);
		return true;
	} else {
		return false;
	}
}

void CSharpLanguage::_on_scripts_domain_about_to_unload() {
	for (KeyValue<Object *, CSharpScriptBinding> &E : script_bindings) {
		CSharpScriptBinding &script_binding = E.value;
		script_binding.gchandle.release();
		script_binding.inited = false;
	}

#ifdef GD_MONO_HOT_RELOAD
	{
		MutexLock lock(ManagedCallable::instances_mutex);

		for (SelfList<ManagedCallable> *elem = ManagedCallable::instances.first(); elem; elem = elem->next()) {
			ManagedCallable *managed_callable = elem->self();
			managed_callable->release_delegate_handle();
		}
	}
#endif
}

#ifdef TOOLS_ENABLED
void CSharpLanguage::_editor_init_callback() {
	// Load GodotTools and initialize GodotSharpEditor

	Object *editor_plugin_obj = GDMono::get_singleton()->plugin_callbacks.LoadToolsAssemblyCallback(
			GodotSharpDirs::get_data_editor_tools_dir().plus_file("GodotTools.dll").utf16());
	CRASH_COND(editor_plugin_obj == nullptr);

	EditorPlugin *godotsharp_editor = Object::cast_to<EditorPlugin>(editor_plugin_obj);
	CRASH_COND(godotsharp_editor == nullptr);

	// Add plugin to EditorNode and enable it
	EditorNode::add_editor_plugin(godotsharp_editor);
	ED_SHORTCUT("mono/build_solution", TTR("Build Solution"), KeyModifierMask::ALT | Key::B);
	godotsharp_editor->enable_plugin();

	get_singleton()->godotsharp_editor = godotsharp_editor;
}
#endif

void CSharpLanguage::set_language_index(int p_idx) {
	ERR_FAIL_COND(lang_idx != -1);
	lang_idx = p_idx;
}

void CSharpLanguage::release_script_gchandle(MonoGCHandleData &p_gchandle) {
	if (!p_gchandle.is_released()) { // Do not lock unnecessarily
		MutexLock lock(get_singleton()->script_gchandle_release_mutex);
		p_gchandle.release();
	}
}

void CSharpLanguage::release_script_gchandle(void *p_expected_mono_obj_unused, MonoGCHandleData &p_gchandle) {
#warning KNOWN BUG. DO NOT USE THIS IN PRODUCTION
	// KNOWN BUG:
	//  I removed the patch from commit e558e1ec09aa27852426bbd24dfa21e9b60cfbfc.
	//  This may cause data races. Re-implementing it without the Mono embedding API would be
	//  too painful and would make the code even more of a mess than it already was.
	//  We will switch from scripts to the new extension system before a release with .NET 6 support.
	//  The problem the old patch was working around won't be present at all with the new extension system.

	(void)p_expected_mono_obj_unused;
	return release_script_gchandle(p_gchandle);
}

CSharpLanguage::CSharpLanguage() {
	ERR_FAIL_COND_MSG(singleton, "C# singleton already exist.");
	singleton = this;
}

CSharpLanguage::~CSharpLanguage() {
	finalize();
	singleton = nullptr;
}

bool CSharpLanguage::setup_csharp_script_binding(CSharpScriptBinding &r_script_binding, Object *p_object) {
#ifdef DEBUG_ENABLED
	// I don't trust you
	if (p_object->get_script_instance()) {
		CSharpInstance *csharp_instance = CAST_CSHARP_INSTANCE(p_object->get_script_instance());
		CRASH_COND(csharp_instance != nullptr && !csharp_instance->is_destructing_script_instance());
	}
#endif

	StringName type_name = p_object->get_class_name();

	// ¯\_(ツ)_/¯
	const ClassDB::ClassInfo *classinfo = ClassDB::classes.getptr(type_name);
	while (classinfo && !classinfo->exposed) {
		classinfo = classinfo->inherits_ptr;
	}
	ERR_FAIL_NULL_V(classinfo, false);
	type_name = classinfo->name;

	bool parent_is_object_class = ClassDB::is_parent_class(p_object->get_class_name(), type_name);
	ERR_FAIL_COND_V_MSG(!parent_is_object_class, false,
			"Type inherits from native type '" + type_name + "', so it can't be instantiated in object of type: '" + p_object->get_class() + "'.");

	GCHandleIntPtr strong_gchandle =
			GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectBinding(&type_name, p_object);

	ERR_FAIL_NULL_V(strong_gchandle.value, false);

	r_script_binding.inited = true;
	r_script_binding.type_name = type_name;
	r_script_binding.gchandle = MonoGCHandleData(strong_gchandle, gdmono::GCHandleType::STRONG_HANDLE);
	r_script_binding.owner = p_object;

	// Tie managed to unmanaged
	RefCounted *rc = Object::cast_to<RefCounted>(p_object);

	if (rc) {
		// Unsafe refcount increment. The managed instance also counts as a reference.
		// This way if the unmanaged world has no references to our owner
		// but the managed instance is alive, the refcount will be 1 instead of 0.
		// See: godot_icall_RefCounted_Dtor(MonoObject *p_obj, Object *p_ptr)

		rc->reference();
		CSharpLanguage::get_singleton()->post_unsafe_reference(rc);
	}

	return true;
}

RBMap<Object *, CSharpScriptBinding>::Element *CSharpLanguage::insert_script_binding(Object *p_object, const CSharpScriptBinding &p_script_binding) {
	return script_bindings.insert(p_object, p_script_binding);
}

void *CSharpLanguage::_instance_binding_create_callback(void *, void *p_instance) {
	CSharpLanguage *csharp_lang = CSharpLanguage::get_singleton();

	MutexLock lock(csharp_lang->language_bind_mutex);

	RBMap<Object *, CSharpScriptBinding>::Element *match = csharp_lang->script_bindings.find((Object *)p_instance);
	if (match) {
		return (void *)match;
	}

	CSharpScriptBinding script_binding;

	return (void *)csharp_lang->insert_script_binding((Object *)p_instance, script_binding);
}

void CSharpLanguage::_instance_binding_free_callback(void *, void *, void *p_binding) {
	CSharpLanguage *csharp_lang = CSharpLanguage::get_singleton();

	if (GDMono::get_singleton() == nullptr) {
#ifdef DEBUG_ENABLED
		CRASH_COND(csharp_lang && !csharp_lang->script_bindings.is_empty());
#endif
		// Mono runtime finalized, all the gchandle bindings were already released
		return;
	}

	if (csharp_lang->finalizing) {
		return; // inside CSharpLanguage::finish(), all the gchandle bindings are released there
	}

	{
		MutexLock lock(csharp_lang->language_bind_mutex);

		RBMap<Object *, CSharpScriptBinding>::Element *data = (RBMap<Object *, CSharpScriptBinding>::Element *)p_binding;

		CSharpScriptBinding &script_binding = data->value();

		if (script_binding.inited) {
			// Set the native instance field to IntPtr.Zero, if not yet garbage collected.
			// This is done to avoid trying to dispose the native instance from Dispose(bool).
			GDMonoCache::managed_callbacks.ScriptManagerBridge_SetGodotObjectPtr(
					script_binding.gchandle.get_intptr(), nullptr);

			script_binding.gchandle.release();
			script_binding.inited = false;
		}

		csharp_lang->script_bindings.erase(data);
	}
}

GDNativeBool CSharpLanguage::_instance_binding_reference_callback(void *p_token, void *p_binding, GDNativeBool p_reference) {
	CRASH_COND(!p_binding);

	CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)p_binding)->get();

	RefCounted *rc_owner = Object::cast_to<RefCounted>(script_binding.owner);

#ifdef DEBUG_ENABLED
	CRASH_COND(!rc_owner);
#endif

	MonoGCHandleData &gchandle = script_binding.gchandle;

	int refcount = rc_owner->reference_get_count();

	if (!script_binding.inited) {
		return refcount == 0;
	}

	if (p_reference) {
		// Refcount incremented
		if (refcount > 1 && gchandle.is_weak()) { // The managed side also holds a reference, hence 1 instead of 0
			// The reference count was increased after the managed side was the only one referencing our owner.
			// This means the owner is being referenced again by the unmanaged side,
			// so the owner must hold the managed side alive again to avoid it from being GCed.

			// Release the current weak handle and replace it with a strong handle.

			GCHandleIntPtr old_gchandle = gchandle.get_intptr();
			gchandle.handle = GCHandleIntPtr(); // No longer owns the handle (released by swap function)

			GCHandleIntPtr new_gchandle;
			bool create_weak = false;
			bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
					old_gchandle, &new_gchandle, create_weak);

			if (!target_alive) {
				return false; // Called after the managed side was collected, so nothing to do here
			}

			gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::STRONG_HANDLE);
		}

		return false;
	} else {
		// Refcount decremented
		if (refcount == 1 && !gchandle.is_released() && !gchandle.is_weak()) { // The managed side also holds a reference, hence 1 instead of 0
			// If owner owner is no longer referenced by the unmanaged side,
			// the managed instance takes responsibility of deleting the owner when GCed.

			// Release the current strong handle and replace it with a weak handle.

			GCHandleIntPtr old_gchandle = gchandle.get_intptr();
			gchandle.handle = GCHandleIntPtr(); // No longer owns the handle (released by swap function)

			GCHandleIntPtr new_gchandle;
			bool create_weak = true;
			bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
					old_gchandle, &new_gchandle, create_weak);

			if (!target_alive) {
				return refcount == 0; // Called after the managed side was collected, so nothing to do here
			}

			gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::WEAK_HANDLE);

			return false;
		}

		return refcount == 0;
	}
}

void *CSharpLanguage::get_instance_binding(Object *p_object) {
	void *binding = p_object->get_instance_binding(get_singleton(), &_instance_binding_callbacks);

	// Initially this was in `_instance_binding_create_callback`. However, after the new instance
	// binding re-write it was resulting in a deadlock in `_instance_binding_reference`, as
	// `setup_csharp_script_binding` may call `reference()`. It was moved here outside to fix that.

	if (binding) {
		CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)binding)->value();

		if (!script_binding.inited) {
			MutexLock lock(CSharpLanguage::get_singleton()->get_language_bind_mutex());

			if (!script_binding.inited) { // Another thread may have set it up
				CSharpLanguage::get_singleton()->setup_csharp_script_binding(script_binding, p_object);
			}
		}
	}

	return binding;
}

void *CSharpLanguage::get_existing_instance_binding(Object *p_object) {
#ifdef DEBUG_ENABLED
	CRASH_COND(p_object->has_instance_binding(p_object));
#endif
	return p_object->get_instance_binding(get_singleton(), &_instance_binding_callbacks);
}

void CSharpLanguage::set_instance_binding(Object *p_object, void *p_binding) {
	p_object->set_instance_binding(get_singleton(), p_binding, &_instance_binding_callbacks);
}

bool CSharpLanguage::has_instance_binding(Object *p_object) {
	return p_object->has_instance_binding(get_singleton());
}
void CSharpLanguage::tie_native_managed_to_unmanaged(GCHandleIntPtr p_gchandle_intptr, Object *p_unmanaged, const StringName *p_native_name, bool p_ref_counted) {
	// This method should not fail

	CRASH_COND(!p_unmanaged);

	// All mono objects created from the managed world (e.g.: 'new Player()')
	// need to have a CSharpScript in order for their methods to be callable from the unmanaged side

	RefCounted *rc = Object::cast_to<RefCounted>(p_unmanaged);

	CRASH_COND(p_ref_counted != (bool)rc);

	MonoGCHandleData gchandle = MonoGCHandleData(p_gchandle_intptr,
			p_ref_counted ? gdmono::GCHandleType::WEAK_HANDLE : gdmono::GCHandleType::STRONG_HANDLE);

	// If it's just a wrapper Godot class and not a custom inheriting class, then attach a
	// script binding instead. One of the advantages of this is that if a script is attached
	// later and it's not a C# script, then the managed object won't have to be disposed.
	// Another reason for doing this is that this instance could outlive CSharpLanguage, which would
	// be problematic when using a script. See: https://github.com/godotengine/godot/issues/25621

	CSharpScriptBinding script_binding;

	script_binding.inited = true;
	script_binding.type_name = *p_native_name;
	script_binding.gchandle = gchandle;
	script_binding.owner = p_unmanaged;

	if (p_ref_counted) {
		// Unsafe refcount increment. The managed instance also counts as a reference.
		// This way if the unmanaged world has no references to our owner
		// but the managed instance is alive, the refcount will be 1 instead of 0.
		// See: godot_icall_RefCounted_Dtor(MonoObject *p_obj, Object *p_ptr)

		// May not me referenced yet, so we must use init_ref() instead of reference()
		if (rc->init_ref()) {
			CSharpLanguage::get_singleton()->post_unsafe_reference(rc);
		}
	}

	// The object was just created, no script instance binding should have been attached
	CRASH_COND(CSharpLanguage::has_instance_binding(p_unmanaged));

	void *data;
	{
		MutexLock lock(CSharpLanguage::get_singleton()->get_language_bind_mutex());
		data = (void *)CSharpLanguage::get_singleton()->insert_script_binding(p_unmanaged, script_binding);
	}

	// Should be thread safe because the object was just created and nothing else should be referencing it
	CSharpLanguage::set_instance_binding(p_unmanaged, data);
}

void CSharpLanguage::tie_user_managed_to_unmanaged(GCHandleIntPtr p_gchandle_intptr, Object *p_unmanaged, CSharpScript *p_script, bool p_ref_counted) {
	// This method should not fail

	CRASH_COND(!p_unmanaged);

	// All mono objects created from the managed world (e.g.: 'new Player()')
	// need to have a CSharpScript in order for their methods to be callable from the unmanaged side

	RefCounted *rc = Object::cast_to<RefCounted>(p_unmanaged);

	CRASH_COND(p_ref_counted != (bool)rc);

	MonoGCHandleData gchandle = MonoGCHandleData(p_gchandle_intptr,
			p_ref_counted ? gdmono::GCHandleType::WEAK_HANDLE : gdmono::GCHandleType::STRONG_HANDLE);

	Ref<CSharpScript> script = p_script;

	CSharpScript::initialize_for_managed_type(script);

	CRASH_COND(script.is_null());

	CSharpInstance *csharp_instance = CSharpInstance::create_for_managed_type(p_unmanaged, script.ptr(), gchandle);

	p_unmanaged->set_script_and_instance(script, csharp_instance);
}

void CSharpLanguage::tie_managed_to_unmanaged_with_pre_setup(GCHandleIntPtr p_gchandle_intptr, Object *p_unmanaged) {
	// This method should not fail

	CRASH_COND(!p_unmanaged);

	CSharpInstance *instance = CAST_CSHARP_INSTANCE(p_unmanaged->get_script_instance());

	if (!instance) {
		return;
	}

	CRASH_COND(!instance->gchandle.is_released());

	// Tie managed to unmanaged
	instance->gchandle = MonoGCHandleData(p_gchandle_intptr, gdmono::GCHandleType::STRONG_HANDLE);

	if (instance->base_ref_counted) {
		instance->_reference_owner_unsafe(); // Here, after assigning the gchandle (for the refcount_incremented callback)
	}

	{
		MutexLock lock(CSharpLanguage::get_singleton()->get_script_instances_mutex());
		// instances is a set, so it's safe to insert multiple times (e.g.: from _internal_new_managed)
		instance->script->instances.insert(instance->owner);
	}
}

CSharpInstance *CSharpInstance::create_for_managed_type(Object *p_owner, CSharpScript *p_script, const MonoGCHandleData &p_gchandle) {
	CSharpInstance *instance = memnew(CSharpInstance(Ref<CSharpScript>(p_script)));

	RefCounted *rc = Object::cast_to<RefCounted>(p_owner);

	instance->base_ref_counted = rc != nullptr;
	instance->owner = p_owner;
	instance->gchandle = p_gchandle;

	if (instance->base_ref_counted) {
		instance->_reference_owner_unsafe();
	}

	p_script->instances.insert(p_owner);

	return instance;
}

Object *CSharpInstance::get_owner() {
	return owner;
}

bool CSharpInstance::set(const StringName &p_name, const Variant &p_value) {
	ERR_FAIL_COND_V(!script.is_valid(), false);

	return GDMonoCache::managed_callbacks.CSharpInstanceBridge_Set(
			gchandle.get_intptr(), &p_name, &p_value);
}

bool CSharpInstance::get(const StringName &p_name, Variant &r_ret) const {
	ERR_FAIL_COND_V(!script.is_valid(), false);

	Variant ret_value;

	bool ret = GDMonoCache::managed_callbacks.CSharpInstanceBridge_Get(
			gchandle.get_intptr(), &p_name, &ret_value);

	if (ret) {
		r_ret = ret_value;
		return true;
	}

	return false;
}

#warning TODO
#if 0
void CSharpInstance::get_properties_state_for_reloading(List<Pair<StringName, Variant>> &r_state) {
	List<PropertyInfo> property_list;
	get_property_list(&property_list);

	for (const PropertyInfo &prop_info : property_list) {
		Pair<StringName, Variant> state_pair;
		state_pair.first = prop_info.name;

		ManagedType managedType;

		GDMonoField *field = nullptr;
		GDMonoClass *top = script->script_class;
		while (top && top != script->native) {
			field = top->get_field(state_pair.first);
			if (field) {
				break;
			}

			top = top->get_parent_class();
		}
		if (!field) {
			continue; // Properties ignored. We get the property baking fields instead.
		}

		managedType = field->get_type();

		if (GDMonoMarshal::managed_to_variant_type(managedType) != Variant::NIL) { // If we can marshal it
			if (get(state_pair.first, state_pair.second)) {
				r_state.push_back(state_pair);
			}
		}
	}
}

void CSharpInstance::get_event_signals_state_for_reloading(List<Pair<StringName, Array>> &r_state) {
	MonoObject *owner_managed = get_mono_object();
	ERR_FAIL_NULL(owner_managed);

	for (const KeyValue<StringName, GDMonoField *> &E : script->event_signals) {
		GDMonoField *event_signal_field = E.value;

		MonoDelegate *delegate_field_value = (MonoDelegate *)event_signal_field->get_value(owner_managed);
		if (!delegate_field_value) {
			continue; // Empty
		}

		Array serialized_data;
		MonoObject *managed_serialized_data = GDMonoMarshal::variant_to_mono_object(serialized_data);

		MonoException *exc = nullptr;
		bool success = (bool)GDMonoCache::managed_callbacks.methodthunk_DelegateUtils_TrySerializeDelegate
							   .invoke(delegate_field_value, managed_serialized_data, &exc);

		if (exc) {
			GDMonoUtils::debug_print_unhandled_exception(exc);
			continue;
		}

		if (success) {
			r_state.push_back(Pair<StringName, Array>(event_signal_field->get_name(), serialized_data));
		} else if (OS::get_singleton()->is_stdout_verbose()) {
			OS::get_singleton()->print("Failed to serialize event signal delegate\n");
		}
	}
}
#endif

void CSharpInstance::get_property_list(List<PropertyInfo> *p_properties) const {
	List<PropertyInfo> props;
	script->get_script_property_list(&props);

	// Call _get_property_list

	ERR_FAIL_COND(!script.is_valid());

	StringName method = SNAME("_get_property_list");

	Variant ret;
	Callable::CallError call_error;
	bool ok = GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &method, nullptr, 0, &call_error, &ret);

	ERR_FAIL_COND_MSG(call_error.error != Callable::CallError::CALL_OK,
			"Error calling '_get_property_list': " + Variant::get_call_error_text(method, nullptr, 0, call_error));

	ERR_FAIL_COND_MSG(!ok, "Unexpected error calling '_get_property_list'");

	Array array = ret;
	for (int i = 0, size = array.size(); i < size; i++) {
		p_properties->push_back(PropertyInfo::from_dict(array.get(i)));
	}

	for (const PropertyInfo &prop : props) {
		p_properties->push_back(prop);
	}
}

Variant::Type CSharpInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	if (script->member_info.has(p_name)) {
		if (r_is_valid) {
			*r_is_valid = true;
		}
		return script->member_info[p_name].type;
	}

	if (r_is_valid) {
		*r_is_valid = false;
	}

	return Variant::NIL;
}

void CSharpInstance::get_method_list(List<MethodInfo> *p_list) const {
#warning TODO
#if 0
	if (!script->is_valid() || !script->script_class) {
		return;
	}

	GD_MONO_SCOPE_THREAD_ATTACH;

	// TODO: We're filtering out constructors but there may be other methods unsuitable for explicit calls.
	GDMonoClass *top = script->script_class;

	while (top && top != script->native) {
		const Vector<GDMonoMethod *> &methods = top->get_all_methods();
		for (int i = 0; i < methods.size(); ++i) {
			MethodInfo minfo = methods[i]->get_method_info();
			if (minfo.name != CACHED_STRING_NAME(dotctor)) {
				p_list->push_back(minfo);
			}
		}

		top = top->get_parent_class();
	}
#endif
}

bool CSharpInstance::has_method(const StringName &p_method) const {
	if (!script.is_valid()) {
		return false;
	}

	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	String method = p_method;
	bool deep = true;

	return GDMonoCache::managed_callbacks.ScriptManagerBridge_HasMethodUnknownParams(
			script.ptr(), &method, deep);
}

Variant CSharpInstance::callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	ERR_FAIL_COND_V(!script.is_valid(), Variant());

	Variant ret;
	GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &p_method, p_args, p_argcount, &r_error, &ret);

	return ret;
}

bool CSharpInstance::_reference_owner_unsafe() {
#ifdef DEBUG_ENABLED
	CRASH_COND(!base_ref_counted);
	CRASH_COND(owner == nullptr);
	CRASH_COND(unsafe_referenced); // already referenced
#endif

	// Unsafe refcount increment. The managed instance also counts as a reference.
	// This way if the unmanaged world has no references to our owner
	// but the managed instance is alive, the refcount will be 1 instead of 0.
	// See: _unreference_owner_unsafe()

	// May not me referenced yet, so we must use init_ref() instead of reference()
	if (static_cast<RefCounted *>(owner)->init_ref()) {
		CSharpLanguage::get_singleton()->post_unsafe_reference(owner);
		unsafe_referenced = true;
	}

	return unsafe_referenced;
}

bool CSharpInstance::_unreference_owner_unsafe() {
#ifdef DEBUG_ENABLED
	CRASH_COND(!base_ref_counted);
	CRASH_COND(owner == nullptr);
#endif

	if (!unsafe_referenced) {
		return false; // Already unreferenced
	}

	unsafe_referenced = false;

	// Called from CSharpInstance::mono_object_disposed() or ~CSharpInstance()

	// Unsafe refcount decrement. The managed instance also counts as a reference.
	// See: _reference_owner_unsafe()

	// Destroying the owner here means self destructing, so we defer the owner destruction to the caller.
	CSharpLanguage::get_singleton()->pre_unsafe_unreference(owner);
	return static_cast<RefCounted *>(owner)->unreference();
}

bool CSharpInstance::_internal_new_managed() {
	CSharpLanguage::get_singleton()->release_script_gchandle(gchandle);

	ERR_FAIL_NULL_V(owner, false);
	ERR_FAIL_COND_V(script.is_null(), false);

	bool ok = GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectScriptInstance(
			script.ptr(), owner, nullptr, 0);

	if (!ok) {
		// Important to clear this before destroying the script instance here
		script = Ref<CSharpScript>();
		owner = nullptr;

		return false;
	}

	CRASH_COND(gchandle.is_released());

	return true;
}

void CSharpInstance::mono_object_disposed() {
	// Must make sure event signals are not left dangling
	disconnect_event_signals();

#ifdef DEBUG_ENABLED
	CRASH_COND(base_ref_counted);
	CRASH_COND(gchandle.is_released());
#endif
	CSharpLanguage::get_singleton()->release_script_gchandle(nullptr, gchandle);
}

void CSharpInstance::mono_object_disposed_baseref(bool p_is_finalizer, bool &r_delete_owner, bool &r_remove_script_instance) {
#ifdef DEBUG_ENABLED
	CRASH_COND(!base_ref_counted);
	CRASH_COND(gchandle.is_released());
#endif

	// Must make sure event signals are not left dangling
	disconnect_event_signals();

	r_remove_script_instance = false;

	if (_unreference_owner_unsafe()) {
		// Safe to self destruct here with memdelete(owner), but it's deferred to the caller to prevent future mistakes.
		r_delete_owner = true;
	} else {
		r_delete_owner = false;
		CSharpLanguage::get_singleton()->release_script_gchandle(nullptr, gchandle);

		if (!p_is_finalizer) {
			// If the native instance is still alive and Dispose() was called
			// (instead of the finalizer), then we remove the script instance.
			r_remove_script_instance = true;
		} else if (!GDMono::get_singleton()->is_finalizing_scripts_domain()) {
			// If the native instance is still alive and this is called from the finalizer,
			// then it was referenced from another thread before the finalizer could
			// unreference and delete it, so we want to keep it.
			// GC.ReRegisterForFinalize(this) is not safe because the objects referenced by 'this'
			// could have already been collected. Instead we will create a new managed instance here.
			if (!_internal_new_managed()) {
				r_remove_script_instance = true;
			}
		}
	}
}

void CSharpInstance::connect_event_signal(const StringName &p_event_signal) {
	// TODO: Use pooling for ManagedCallable instances.
	EventSignalCallable *event_signal_callable = memnew(EventSignalCallable(owner, p_event_signal));

	Callable callable(event_signal_callable);
	connected_event_signals.push_back(callable);
	owner->connect(p_event_signal, callable);
}

void CSharpInstance::disconnect_event_signals() {
	for (const Callable &callable : connected_event_signals) {
		const EventSignalCallable *event_signal_callable = static_cast<const EventSignalCallable *>(callable.get_custom());
		owner->disconnect(event_signal_callable->get_signal(), callable);
	}

	connected_event_signals.clear();
}

void CSharpInstance::refcount_incremented() {
#ifdef DEBUG_ENABLED
	CRASH_COND(!base_ref_counted);
	CRASH_COND(owner == nullptr);
#endif

	RefCounted *rc_owner = Object::cast_to<RefCounted>(owner);

	if (rc_owner->reference_get_count() > 1 && gchandle.is_weak()) { // The managed side also holds a reference, hence 1 instead of 0
		// The reference count was increased after the managed side was the only one referencing our owner.
		// This means the owner is being referenced again by the unmanaged side,
		// so the owner must hold the managed side alive again to avoid it from being GCed.

		// Release the current weak handle and replace it with a strong handle.

		GCHandleIntPtr old_gchandle = gchandle.get_intptr();
		gchandle.handle = GCHandleIntPtr(); // No longer owns the handle (released by swap function)

		GCHandleIntPtr new_gchandle;
		bool create_weak = false;
		bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
				old_gchandle, &new_gchandle, create_weak);

		if (!target_alive) {
			return; // Called after the managed side was collected, so nothing to do here
		}

		gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::STRONG_HANDLE);
	}
}

bool CSharpInstance::refcount_decremented() {
#ifdef DEBUG_ENABLED
	CRASH_COND(!base_ref_counted);
	CRASH_COND(owner == nullptr);
#endif

	RefCounted *rc_owner = Object::cast_to<RefCounted>(owner);

	int refcount = rc_owner->reference_get_count();

	if (refcount == 1 && !gchandle.is_weak()) { // The managed side also holds a reference, hence 1 instead of 0
		// If owner owner is no longer referenced by the unmanaged side,
		// the managed instance takes responsibility of deleting the owner when GCed.

		// Release the current strong handle and replace it with a weak handle.

		GCHandleIntPtr old_gchandle = gchandle.get_intptr();
		gchandle.handle = GCHandleIntPtr(); // No longer owns the handle (released by swap function)

		GCHandleIntPtr new_gchandle;
		bool create_weak = true;
		bool target_alive = GDMonoCache::managed_callbacks.ScriptManagerBridge_SwapGCHandleForType(
				old_gchandle, &new_gchandle, create_weak);

		if (!target_alive) {
			return refcount == 0; // Called after the managed side was collected, so nothing to do here
		}

		gchandle = MonoGCHandleData(new_gchandle, gdmono::GCHandleType::WEAK_HANDLE);

		return false;
	}

	ref_dying = (refcount == 0);

	return ref_dying;
}

const Variant CSharpInstance::get_rpc_config() const {
	return script->get_rpc_config();
}

void CSharpInstance::notification(int p_notification) {
	if (p_notification == Object::NOTIFICATION_PREDELETE) {
		// When NOTIFICATION_PREDELETE is sent, we also take the chance to call Dispose().
		// It's safe to call Dispose() multiple times and NOTIFICATION_PREDELETE is guaranteed
		// to be sent at least once, which happens right before the call to the destructor.

		predelete_notified = true;

		if (base_ref_counted) {
			// It's not safe to proceed if the owner derives RefCounted and the refcount reached 0.
			// At this point, Dispose() was already called (manually or from the finalizer) so
			// that's not a problem. The refcount wouldn't have reached 0 otherwise, since the
			// managed side references it and Dispose() needs to be called to release it.
			// However, this means C# RefCounted scripts can't receive NOTIFICATION_PREDELETE, but
			// this is likely the case with GDScript as well: https://github.com/godotengine/godot/issues/6784
			return;
		}

		_call_notification(p_notification);

		GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose(
				gchandle.get_intptr(), /* okIfNull */ false);

		return;
	}

	_call_notification(p_notification);
}

void CSharpInstance::_call_notification(int p_notification) {
	Variant arg = p_notification;
	const Variant *args[1] = { &arg };
	StringName method_name = SNAME("_notification");

	Callable::CallError call_error;

	Variant ret;
	GDMonoCache::managed_callbacks.CSharpInstanceBridge_Call(
			gchandle.get_intptr(), &method_name, args, 1, &call_error, &ret);
}

String CSharpInstance::to_string(bool *r_valid) {
	String res;
	bool valid;

	GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallToString(
			gchandle.get_intptr(), &res, &valid);

	if (r_valid) {
		*r_valid = valid;
	}

	return res;
}

Ref<Script> CSharpInstance::get_script() const {
	return script;
}

ScriptLanguage *CSharpInstance::get_language() {
	return CSharpLanguage::get_singleton();
}

CSharpInstance::CSharpInstance(const Ref<CSharpScript> &p_script) :
		script(p_script) {
}

CSharpInstance::~CSharpInstance() {
	destructing_script_instance = true;

	// Must make sure event signals are not left dangling
	disconnect_event_signals();

	if (!gchandle.is_released()) {
		if (!predelete_notified && !ref_dying) {
			// This destructor is not called from the owners destructor.
			// This could be being called from the owner's set_script_instance method,
			// meaning this script is being replaced with another one. If this is the case,
			// we must call Dispose here, because Dispose calls owner->set_script_instance(nullptr)
			// and that would mess up with the new script instance if called later.

			GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose(
					gchandle.get_intptr(), /* okIfNull */ true);
		}

		gchandle.release(); // Make sure the gchandle is released
	}

	// If not being called from the owner's destructor, and we still hold a reference to the owner
	if (base_ref_counted && !ref_dying && owner && unsafe_referenced) {
		// The owner's script or script instance is being replaced (or removed)

		// Transfer ownership to an "instance binding"

		RefCounted *rc_owner = static_cast<RefCounted *>(owner);

		// We will unreference the owner before referencing it again, so we need to keep it alive
		Ref<RefCounted> scope_keep_owner_alive(rc_owner);
		(void)scope_keep_owner_alive;

		// Unreference the owner here, before the new "instance binding" references it.
		// Otherwise, the unsafe reference debug checks will incorrectly detect a bug.
		bool die = _unreference_owner_unsafe();
		CRASH_COND(die); // `owner_keep_alive` holds a reference, so it can't die

		void *data = CSharpLanguage::get_instance_binding(owner);
		CRASH_COND(data == nullptr);
		CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)data)->get();
		CRASH_COND(!script_binding.inited);

#ifdef DEBUG_ENABLED
		// The "instance binding" holds a reference so the refcount should be at least 2 before `scope_keep_owner_alive` goes out of scope
		CRASH_COND(rc_owner->reference_get_count() <= 1);
#endif
	}

	if (script.is_valid() && owner) {
		MutexLock lock(CSharpLanguage::get_singleton()->script_instances_mutex);

#ifdef DEBUG_ENABLED
		// CSharpInstance must not be created unless it's going to be added to the list for sure
		HashSet<Object *>::Iterator match = script->instances.find(owner);
		CRASH_COND(!match);
		script->instances.remove(match);
#else
		script->instances.erase(owner);
#endif
	}
}

#ifdef TOOLS_ENABLED
void CSharpScript::_placeholder_erased(PlaceHolderScriptInstance *p_placeholder) {
	placeholders.erase(p_placeholder);
}
#endif

#ifdef TOOLS_ENABLED
void CSharpScript::_update_exports_values(HashMap<StringName, Variant> &values, List<PropertyInfo> &propnames) {
	for (const KeyValue<StringName, Variant> &E : exported_members_defval_cache) {
		values[E.key] = E.value;
	}

	for (const PropertyInfo &prop_info : exported_members_cache) {
		propnames.push_back(prop_info);
	}

	if (base_cache.is_valid()) {
		base_cache->_update_exports_values(values, propnames);
	}
}

void CSharpScript::_update_member_info_no_exports() {
	if (exports_invalidated) {
		exports_invalidated = false;

		member_info.clear();

#warning TODO
#if 0
		GDMonoClass *top = script_class;
		List<PropertyInfo> props;

		while (top && top != native) {
			PropertyInfo prop_info;
			bool exported;

			const Vector<GDMonoField *> &fields = top->get_all_fields();

			for (int i = fields.size() - 1; i >= 0; i--) {
				GDMonoField *field = fields[i];

				if (_get_member_export(field, /* inspect export: */ false, prop_info, exported)) {
					StringName member_name = field->get_name();

					member_info[member_name] = prop_info;
					props.push_front(prop_info);
					exported_members_defval_cache[member_name] = Variant();
				}
			}

			const Vector<GDMonoProperty *> &properties = top->get_all_properties();

			for (int i = properties.size() - 1; i >= 0; i--) {
				GDMonoProperty *property = properties[i];

				if (_get_member_export(property, /* inspect export: */ false, prop_info, exported)) {
					StringName member_name = property->get_name();

					member_info[member_name] = prop_info;
					props.push_front(prop_info);
					exported_members_defval_cache[member_name] = Variant();
				}
			}

			exported_members_cache.push_back(PropertyInfo(Variant::NIL, top->get_name(), PROPERTY_HINT_NONE, get_path(), PROPERTY_USAGE_CATEGORY));
			for (const PropertyInfo &E : props) {
				exported_members_cache.push_back(E);
			}

			props.clear();

			top = top->get_parent_class();
		}
#endif
	}
}
#endif

bool CSharpScript::_update_exports(PlaceHolderScriptInstance *p_instance_to_update) {
#ifdef TOOLS_ENABLED
	bool is_editor = Engine::get_singleton()->is_editor_hint();
	if (is_editor) {
		placeholder_fallback_enabled = true; // until proven otherwise
	}
#endif
	if (!valid) {
		return false;
	}

	bool changed = false;

#ifdef TOOLS_ENABLED
	if (exports_invalidated)
#endif
	{
#warning TODO
#if 0
		GD_MONO_SCOPE_THREAD_ATTACH;

		changed = true;

		member_info.clear();

#ifdef TOOLS_ENABLED
		MonoObject *tmp_object = nullptr;
		Object *tmp_native = nullptr;
		uint32_t tmp_pinned_gchandle = 0;

		if (is_editor) {
			exports_invalidated = false;

			exported_members_cache.clear();
			exported_members_defval_cache.clear();

			// Here we create a temporary managed instance of the class to get the initial values
			tmp_object = mono_object_new(mono_domain_get(), script_class->get_mono_ptr());

			if (!tmp_object) {
				ERR_PRINT("Failed to allocate temporary MonoObject.");
				return false;
			}

			tmp_pinned_gchandle = GDMonoUtils::new_strong_gchandle_pinned(tmp_object); // pin it (not sure if needed)

			GDMonoMethod *ctor = script_class->get_method(CACHED_STRING_NAME(dotctor), 0);

			ERR_FAIL_NULL_V_MSG(ctor, false,
					"Cannot construct temporary MonoObject because the class does not define a parameterless constructor: '" + get_path() + "'.");

			MonoException *ctor_exc = nullptr;
			ctor->invoke(tmp_object, nullptr, &ctor_exc);

			tmp_native = GDMonoMarshal::unbox<Object *>(GDMonoCache::cached_data.field_GodotObject_ptr->get_value(tmp_object));

			if (ctor_exc) {
				// TODO: Should we free 'tmp_native' if the exception was thrown after its creation?

				GDMonoUtils::free_gchandle(tmp_pinned_gchandle);
				tmp_object = nullptr;

				ERR_PRINT("Exception thrown from constructor of temporary MonoObject:");
				GDMonoUtils::debug_print_unhandled_exception(ctor_exc);
				return false;
			}
		}
#endif

		GDMonoClass *top = script_class;
		List<PropertyInfo> props;

		while (top && top != native) {
			PropertyInfo prop_info;
			bool exported;

			const Vector<GDMonoField *> &fields = top->get_all_fields();

			for (int i = fields.size() - 1; i >= 0; i--) {
				GDMonoField *field = fields[i];

				if (_get_member_export(field, /* inspect export: */ true, prop_info, exported)) {
					StringName member_name = field->get_name();

					member_info[member_name] = prop_info;

					if (exported) {
#ifdef TOOLS_ENABLED
						if (is_editor) {
							props.push_front(prop_info);

							if (tmp_object) {
								exported_members_defval_cache[member_name] = GDMonoMarshal::mono_object_to_variant(field->get_value(tmp_object));
							}
						}
#endif

#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
						exported_members_names.insert(member_name);
#endif
					}
				}
			}

			const Vector<GDMonoProperty *> &properties = top->get_all_properties();

			for (int i = properties.size() - 1; i >= 0; i--) {
				GDMonoProperty *property = properties[i];

				if (_get_member_export(property, /* inspect export: */ true, prop_info, exported)) {
					StringName member_name = property->get_name();

					member_info[member_name] = prop_info;

					if (exported) {
#ifdef TOOLS_ENABLED
						if (is_editor) {
							props.push_front(prop_info);
							if (tmp_object) {
								MonoException *exc = nullptr;
								MonoObject *ret = property->get_value(tmp_object, &exc);
								if (exc) {
									exported_members_defval_cache[member_name] = Variant();
									GDMonoUtils::debug_print_unhandled_exception(exc);
								} else {
									exported_members_defval_cache[member_name] = GDMonoMarshal::mono_object_to_variant(ret);
								}
							}
						}
#endif

#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
						exported_members_names.insert(member_name);
#endif
					}
				}
			}

#ifdef TOOLS_ENABLED
			exported_members_cache.push_back(PropertyInfo(Variant::NIL, top->get_name(), PROPERTY_HINT_NONE, get_path(), PROPERTY_USAGE_CATEGORY));

			for (const PropertyInfo &E : props) {
				exported_members_cache.push_back(E);
			}

			props.clear();
#endif // TOOLS_ENABLED

			top = top->get_parent_class();
		}

#ifdef TOOLS_ENABLED
		if (is_editor) {
			// Need to check this here, before disposal
			bool base_ref_counted = Object::cast_to<RefCounted>(tmp_native) != nullptr;

			// Dispose the temporary managed instance

			MonoException *exc = nullptr;
			GDMonoUtils::dispose(tmp_object, &exc);

			if (exc) {
				ERR_PRINT("Exception thrown from method Dispose() of temporary MonoObject:");
				GDMonoUtils::debug_print_unhandled_exception(exc);
			}

			GDMonoUtils::free_gchandle(tmp_pinned_gchandle);
			tmp_object = nullptr;

			if (tmp_native && !base_ref_counted) {
				Node *node = Object::cast_to<Node>(tmp_native);
				if (node && node->is_inside_tree()) {
					ERR_PRINT("Temporary instance was added to the scene tree.");
				} else {
					memdelete(tmp_native);
				}
			}
		}
#endif

#endif // #if 0
	}

#ifdef TOOLS_ENABLED
	if (is_editor) {
		placeholder_fallback_enabled = false;

		if ((changed || p_instance_to_update) && placeholders.size()) {
			// Update placeholders if any
			HashMap<StringName, Variant> values;
			List<PropertyInfo> propnames;
			_update_exports_values(values, propnames);

			if (changed) {
				for (PlaceHolderScriptInstance *script_instance : placeholders) {
					script_instance->update(propnames, values);
				}
			} else {
				p_instance_to_update->update(propnames, values);
			}
		}
	}
#endif

	return changed;
}

#warning TODO
#if 0
/**
 * Returns false if there was an error, otherwise true.
 * If there was an error, r_prop_info and r_exported are not assigned any value.
 */
bool CSharpScript::_get_member_export(IMonoClassMember *p_member, bool p_inspect_export, PropertyInfo &r_prop_info, bool &r_exported) {
	GD_MONO_ASSERT_THREAD_ATTACHED;

	// Goddammit, C++. All I wanted was some nested functions.
#define MEMBER_FULL_QUALIFIED_NAME(m_member) \
	(m_member->get_enclosing_class()->get_full_name() + "." + (String)m_member->get_name())

	if (p_member->is_static()) {
#ifdef TOOLS_ENABLED
		if (p_member->has_attribute(GDMonoCache::cached_data.class_ExportAttribute)) {
			ERR_PRINT("Cannot export member because it is static: '" + MEMBER_FULL_QUALIFIED_NAME(p_member) + "'.");
		}
#endif
		return false;
	}

	if (member_info.has(p_member->get_name())) {
		return false;
	}

	ManagedType type;

	if (p_member->get_member_type() == IMonoClassMember::MEMBER_TYPE_FIELD) {
		type = static_cast<GDMonoField *>(p_member)->get_type();
	} else if (p_member->get_member_type() == IMonoClassMember::MEMBER_TYPE_PROPERTY) {
		type = static_cast<GDMonoProperty *>(p_member)->get_type();
	} else {
		CRASH_NOW();
	}

	bool exported = p_member->has_attribute(GDMonoCache::cached_data.class_ExportAttribute);

	if (p_member->get_member_type() == IMonoClassMember::MEMBER_TYPE_PROPERTY) {
		GDMonoProperty *property = static_cast<GDMonoProperty *>(p_member);
		if (!property->has_getter()) {
#ifdef TOOLS_ENABLED
			if (exported) {
				ERR_PRINT("Cannot export a property without a getter: '" + MEMBER_FULL_QUALIFIED_NAME(p_member) + "'.");
			}
#endif
			return false;
		}
		if (!property->has_setter()) {
#ifdef TOOLS_ENABLED
			if (exported) {
				ERR_PRINT("Cannot export a property without a setter: '" + MEMBER_FULL_QUALIFIED_NAME(p_member) + "'.");
			}
#endif
			return false;
		}
	}

	bool nil_is_variant = false;
	Variant::Type variant_type = GDMonoMarshal::managed_to_variant_type(type, &nil_is_variant);

	if (!p_inspect_export || !exported) {
		r_prop_info = PropertyInfo(variant_type, (String)p_member->get_name(), PROPERTY_HINT_NONE, "", PROPERTY_USAGE_SCRIPT_VARIABLE);
		r_exported = false;
		return true;
	}

#ifdef TOOLS_ENABLED
	MonoObject *attr = p_member->get_attribute(GDMonoCache::cached_data.class_ExportAttribute);
#endif

	PropertyHint hint = PROPERTY_HINT_NONE;
	String hint_string;

	if (variant_type == Variant::NIL && !nil_is_variant) {
#ifdef TOOLS_ENABLED
		ERR_PRINT("Unknown exported member type: '" + MEMBER_FULL_QUALIFIED_NAME(p_member) + "'.");
#endif
		return false;
	}

#ifdef TOOLS_ENABLED
	int hint_res = _try_get_member_export_hint(p_member, type, variant_type, /* allow_generics: */ true, hint, hint_string);

	ERR_FAIL_COND_V_MSG(hint_res == -1, false,
			"Error while trying to determine information about the exported member: '" +
					MEMBER_FULL_QUALIFIED_NAME(p_member) + "'.");

	if (hint_res == 0) {
		hint = PropertyHint(GDMonoCache::cached_data.field_ExportAttribute_hint->get_int_value(attr));
		hint_string = GDMonoCache::cached_data.field_ExportAttribute_hintString->get_string_value(attr);
	}
#endif

	uint32_t prop_usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE;

	if (variant_type == Variant::NIL) {
		// System.Object (Variant)
		prop_usage |= PROPERTY_USAGE_NIL_IS_VARIANT;
	}

	r_prop_info = PropertyInfo(variant_type, (String)p_member->get_name(), hint, hint_string, prop_usage);
	r_exported = true;

	return true;

#undef MEMBER_FULL_QUALIFIED_NAME
}

#ifdef TOOLS_ENABLED
int CSharpScript::_try_get_member_export_hint(IMonoClassMember *p_member, ManagedType p_type, Variant::Type p_variant_type, bool p_allow_generics, PropertyHint &r_hint, String &r_hint_string) {
	if (p_variant_type == Variant::NIL) {
		// System.Object (Variant)
		return 1;
	}

	GD_MONO_ASSERT_THREAD_ATTACHED;

	if (p_variant_type == Variant::INT && p_type.type_encoding == MONO_TYPE_VALUETYPE && mono_class_is_enum(p_type.type_class->get_mono_ptr())) {
		MonoReflectionType *reftype = mono_type_get_object(mono_domain_get(), p_type.type_class->get_mono_type());
		r_hint = GDMonoUtils::Marshal::type_has_flags_attribute(reftype) ? PROPERTY_HINT_FLAGS : PROPERTY_HINT_ENUM;

		Vector<MonoClassField *> fields = p_type.type_class->get_enum_fields();

		MonoType *enum_basetype = mono_class_enum_basetype(p_type.type_class->get_mono_ptr());

		String name_only_hint_string;

		// True: enum Foo { Bar, Baz, Quux }
		// True: enum Foo { Bar = 0, Baz = 1, Quux = 2 }
		// False: enum Foo { Bar = 0, Baz = 7, Quux = 5 }
		bool uses_default_values = true;

		for (int i = 0; i < fields.size(); i++) {
			MonoClassField *field = fields[i];

			if (i > 0) {
				r_hint_string += ",";
				name_only_hint_string += ",";
			}

			String enum_field_name = String::utf8(mono_field_get_name(field));
			r_hint_string += enum_field_name;
			name_only_hint_string += enum_field_name;

			// TODO:
			// Instead of using mono_field_get_value_object, we can do this without boxing. Check the
			// internal mono functions: ves_icall_System_Enum_GetEnumValuesAndNames and the get_enum_field.

			MonoObject *val_obj = mono_field_get_value_object(mono_domain_get(), field, nullptr);

			ERR_FAIL_NULL_V_MSG(val_obj, -1, "Failed to get '" + enum_field_name + "' constant enum value.");

			bool r_error;
			uint64_t val = GDMonoUtils::unbox_enum_value(val_obj, enum_basetype, r_error);
			ERR_FAIL_COND_V_MSG(r_error, -1, "Failed to unbox '" + enum_field_name + "' constant enum value.");

			unsigned int expected_val = r_hint == PROPERTY_HINT_FLAGS ? 1 << i : i;
			if (val != expected_val) {
				uses_default_values = false;
			}

			r_hint_string += ":";
			r_hint_string += String::num_uint64(val);
		}

		if (uses_default_values) {
			// If we use the format NAME:VAL, that's what the editor displays.
			// That's annoying if the user is not using custom values for the enum constants.
			// This may not be needed in the future if the editor is changed to not display values.
			r_hint_string = name_only_hint_string;
		}
	} else if (p_variant_type == Variant::OBJECT && GDMonoCache::cached_data.class_GodotResource->is_assignable_from(p_type.type_class)) {
		GDMonoClass *field_native_class = GDMonoUtils::get_class_native_base(p_type.type_class);
		CRASH_COND(field_native_class == nullptr);

		r_hint = PROPERTY_HINT_RESOURCE_TYPE;
		r_hint_string = String(NATIVE_GDMONOCLASS_NAME(field_native_class));
	} else if (p_variant_type == Variant::OBJECT && CACHED_CLASS(Node)->is_assignable_from(p_type.type_class)) {
		GDMonoClass *field_native_class = GDMonoUtils::get_class_native_base(p_type.type_class);
		CRASH_COND(field_native_class == nullptr);

		r_hint = PROPERTY_HINT_NODE_TYPE;
		r_hint_string = String(NATIVE_GDMONOCLASS_NAME(field_native_class));
	} else if (p_allow_generics && p_variant_type == Variant::ARRAY) {
		// Nested arrays are not supported in the inspector

		ManagedType elem_type;

		if (!GDMonoMarshal::try_get_array_element_type(p_type, elem_type)) {
			return 0;
		}

		Variant::Type elem_variant_type = GDMonoMarshal::managed_to_variant_type(elem_type);

		PropertyHint elem_hint = PROPERTY_HINT_NONE;
		String elem_hint_string;

		ERR_FAIL_COND_V_MSG(elem_variant_type == Variant::NIL, -1, "Unknown array element type.");

		bool preset_hint = false;
		if (elem_variant_type == Variant::STRING) {
			MonoObject *attr = p_member->get_attribute(CACHED_CLASS(ExportAttribute));
			if (PropertyHint(CACHED_FIELD(ExportAttribute, hint)->get_int_value(attr)) == PROPERTY_HINT_ENUM) {
				r_hint_string = itos(elem_variant_type) + "/" + itos(PROPERTY_HINT_ENUM) + ":" + CACHED_FIELD(ExportAttribute, hintString)->get_string_value(attr);
				preset_hint = true;
			}
		}

		if (!preset_hint) {
			int hint_res = _try_get_member_export_hint(p_member, elem_type, elem_variant_type, /* allow_generics: */ false, elem_hint, elem_hint_string);

			ERR_FAIL_COND_V_MSG(hint_res == -1, -1, "Error while trying to determine information about the array element type.");

			// Format: type/hint:hint_string
			r_hint_string = itos(elem_variant_type) + "/" + itos(elem_hint) + ":" + elem_hint_string;
		}

		r_hint = PROPERTY_HINT_TYPE_STRING;

	} else if (p_allow_generics && p_variant_type == Variant::DICTIONARY) {
		// TODO: Dictionaries are not supported in the inspector
	} else {
		return 0;
	}

	return 1;
}
#endif
#endif

bool CSharpScript::_get(const StringName &p_name, Variant &r_ret) const {
	if (p_name == CSharpLanguage::singleton->string_names._script_source) {
		r_ret = get_source_code();
		return true;
	}

	return false;
}

bool CSharpScript::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == CSharpLanguage::singleton->string_names._script_source) {
		set_source_code(p_value);
		reload();
		return true;
	}

	return false;
}

void CSharpScript::_get_property_list(List<PropertyInfo> *p_properties) const {
	p_properties->push_back(PropertyInfo(Variant::STRING, CSharpLanguage::singleton->string_names._script_source, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL));
}

void CSharpScript::_bind_methods() {
	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "new", &CSharpScript::_new, MethodInfo("new"));
}

void CSharpScript::initialize_for_managed_type(Ref<CSharpScript> p_script) {
	// IMPORTANT:
	// This method must be called only after the CSharpScript and its associated type
	// have been added to the script bridge map in the ScriptManagerBridge C# class.

	// This method should not fail, only assertions allowed

	p_script->valid = true;
	p_script->reload_invalidated = false;

	update_script_class_info(p_script);

#ifdef TOOLS_ENABLED
	p_script->_update_member_info_no_exports();
#endif
}

// Extract information about the script using the mono class.
void CSharpScript::update_script_class_info(Ref<CSharpScript> p_script) {
	bool tool = false;
	Dictionary rpc_functions_dict;
	// Destructor won't be called from C#, and I don't want to include the GDNative header
	// only for this, so need to call the destructor manually before passing this to C#.
	rpc_functions_dict.~Dictionary();

	GDMonoCache::managed_callbacks.ScriptManagerBridge_UpdateScriptClassInfo(
			p_script.ptr(), &tool, &rpc_functions_dict);

	p_script->tool = tool;

	p_script->rpc_config.clear();
	p_script->rpc_config = rpc_functions_dict;
}

bool CSharpScript::can_instantiate() const {
#ifdef TOOLS_ENABLED
	bool extra_cond = tool || ScriptServer::is_scripting_enabled();
#else
	bool extra_cond = true;
#endif

	// FIXME Need to think this through better.
	// For tool scripts, this will never fire if the class is not found. That's because we
	// don't know if it's a tool script if we can't find the class to access the attributes.
	if (extra_cond && !valid) {
		ERR_FAIL_V_MSG(false, "Cannot instance script because the associated class could not be found. Script: '" + get_path() + "'.");
	}

	return valid && extra_cond;
}

StringName CSharpScript::get_instance_base_type() const {
	StringName native_name;
	GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptNativeName(this, &native_name);
	return native_name;
}

CSharpInstance *CSharpScript::_create_instance(const Variant **p_args, int p_argcount, Object *p_owner, bool p_is_ref_counted, Callable::CallError &r_error) {
	/* STEP 1, CREATE */

	Ref<RefCounted> ref;
	if (p_is_ref_counted) {
		// Hold it alive. Important if we have to dispose a script instance binding before creating the CSharpInstance.
		ref = Ref<RefCounted>(static_cast<RefCounted *>(p_owner));
	}

	// If the object had a script instance binding, dispose it before adding the CSharpInstance
	if (CSharpLanguage::has_instance_binding(p_owner)) {
		void *data = CSharpLanguage::get_existing_instance_binding(p_owner);
		CRASH_COND(data == nullptr);

		CSharpScriptBinding &script_binding = ((RBMap<Object *, CSharpScriptBinding>::Element *)data)->get();
		if (script_binding.inited && !script_binding.gchandle.is_released()) {
			GDMonoCache::managed_callbacks.CSharpInstanceBridge_CallDispose(
					script_binding.gchandle.get_intptr(), /* okIfNull */ true);

			script_binding.gchandle.release(); // Just in case
			script_binding.inited = false;
		}
	}

	CSharpInstance *instance = memnew(CSharpInstance(Ref<CSharpScript>(this)));
	instance->base_ref_counted = p_is_ref_counted;
	instance->owner = p_owner;
	instance->owner->set_script_instance(instance);

	/* STEP 2, INITIALIZE AND CONSTRUCT */

	bool ok = GDMonoCache::managed_callbacks.ScriptManagerBridge_CreateManagedForGodotObjectScriptInstance(
			this, p_owner, p_args, p_argcount);

	if (!ok) {
		// Important to clear this before destroying the script instance here
		instance->script = Ref<CSharpScript>();
		instance->owner = nullptr;
		p_owner->set_script_instance(nullptr);

		return nullptr;
	}

	CRASH_COND(instance->gchandle.is_released());

	/* STEP 3, PARTY */

	//@TODO make thread safe
	return instance;
}

Variant CSharpScript::_new(const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (!valid) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	r_error.error = Callable::CallError::CALL_OK;

	StringName native_name;
	GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptNativeName(this, &native_name);

	ERR_FAIL_COND_V(native_name == StringName(), Variant());

	Object *owner = ClassDB::instantiate(native_name);

	Ref<RefCounted> ref;
	RefCounted *r = Object::cast_to<RefCounted>(owner);
	if (r) {
		ref = Ref<RefCounted>(r);
	}

	CSharpInstance *instance = _create_instance(p_args, p_argcount, owner, r != nullptr, r_error);
	if (!instance) {
		if (ref.is_null()) {
			memdelete(owner); // no owner, sorry
		}
		return Variant();
	}

	if (ref.is_valid()) {
		return ref;
	} else {
		return owner;
	}
}

ScriptInstance *CSharpScript::instance_create(Object *p_this) {
#ifdef DEBUG_ENABLED
	CRASH_COND(!valid);
#endif

	StringName native_name;
	GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptNativeName(this, &native_name);

	ERR_FAIL_COND_V(native_name == StringName(), nullptr);

	if (!ClassDB::is_parent_class(p_this->get_class_name(), native_name)) {
		if (EngineDebugger::is_active()) {
			CSharpLanguage::get_singleton()->debug_break_parse(get_path(), 0,
					"Script inherits from native type '" + String(native_name) +
							"', so it can't be instantiated in object of type: '" + p_this->get_class() + "'");
		}
		ERR_FAIL_V_MSG(nullptr, "Script inherits from native type '" + String(native_name) + "', so it can't be instantiated in object of type: '" + p_this->get_class() + "'.");
	}

	Callable::CallError unchecked_error;
	return _create_instance(nullptr, 0, p_this, Object::cast_to<RefCounted>(p_this) != nullptr, unchecked_error);
}

PlaceHolderScriptInstance *CSharpScript::placeholder_instance_create(Object *p_this) {
#ifdef TOOLS_ENABLED
	PlaceHolderScriptInstance *si = memnew(PlaceHolderScriptInstance(CSharpLanguage::get_singleton(), Ref<Script>(this), p_this));
	placeholders.insert(si);
	_update_exports(si);
	return si;
#else
	return nullptr;
#endif
}

bool CSharpScript::instance_has(const Object *p_this) const {
	MutexLock lock(CSharpLanguage::get_singleton()->script_instances_mutex);
	return instances.has((Object *)p_this);
}

bool CSharpScript::has_source_code() const {
	return !source.is_empty();
}

String CSharpScript::get_source_code() const {
	return source;
}

void CSharpScript::set_source_code(const String &p_code) {
	if (source == p_code) {
		return;
	}
	source = p_code;
#ifdef TOOLS_ENABLED
	source_changed_cache = true;
#endif
}

void CSharpScript::get_script_method_list(List<MethodInfo> *p_list) const {
	if (!valid) {
		return;
	}

#warning TODO
#if 0
	// TODO: We're filtering out constructors but there may be other methods unsuitable for explicit calls.
	GDMonoClass *top = script_class;

	while (top && top != native) {
		const Vector<GDMonoMethod *> &methods = top->get_all_methods();
		for (int i = 0; i < methods.size(); ++i) {
			MethodInfo minfo = methods[i]->get_method_info();
			if (minfo.name != CACHED_STRING_NAME(dotctor)) {
				p_list->push_back(methods[i]->get_method_info());
			}
		}

		top = top->get_parent_class();
	}
#endif
}

bool CSharpScript::has_method(const StringName &p_method) const {
	if (!valid) {
		return false;
	}

	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	String method = p_method;
	bool deep = false;

	bool found = GDMonoCache::managed_callbacks.ScriptManagerBridge_HasMethodUnknownParams(
			this, &method, deep);

	return found;
}

MethodInfo CSharpScript::get_method_info(const StringName &p_method) const {
	if (!valid) {
		return MethodInfo();
	}

#warning TODO
#if 0
	GDMonoClass *top = script_class;

	while (top && top != native) {
		GDMonoMethod *params = top->get_method_unknown_params(p_method);
		if (params) {
			return params->get_method_info();
		}

		top = top->get_parent_class();
	}
#endif

	return MethodInfo();
}

Error CSharpScript::reload(bool p_keep_state) {
	if (!reload_invalidated) {
		return OK;
	}

	// In the case of C#, reload doesn't really do any script reloading.
	// That's done separately via domain reloading.
	reload_invalidated = false;

	String script_path = get_path();

	valid = GDMonoCache::managed_callbacks.ScriptManagerBridge_AddScriptBridge(this, &script_path);

	if (valid) {
#ifdef DEBUG_ENABLED
		print_verbose("Found class for script " + get_path());
#endif

		update_script_class_info(this);

		_update_exports();
	}

	return OK;
}

ScriptLanguage *CSharpScript::get_language() const {
	return CSharpLanguage::get_singleton();
}

bool CSharpScript::get_property_default_value(const StringName &p_property, Variant &r_value) const {
#ifdef TOOLS_ENABLED

	HashMap<StringName, Variant>::ConstIterator E = exported_members_defval_cache.find(p_property);
	if (E) {
		r_value = E->value;
		return true;
	}

	if (base_cache.is_valid()) {
		return base_cache->get_property_default_value(p_property, r_value);
	}

#endif
	return false;
}

void CSharpScript::update_exports() {
#ifdef TOOLS_ENABLED
	_update_exports();
#endif
}

bool CSharpScript::has_script_signal(const StringName &p_signal) const {
	if (!valid) {
		return false;
	}

	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	String signal = p_signal;

	return GDMonoCache::managed_callbacks.ScriptManagerBridge_HasScriptSignal(this, &signal);
}

void CSharpScript::get_script_signal_list(List<MethodInfo> *r_signals) const {
	if (!valid) {
		return;
	}

	// Performance is not critical here as this will be replaced with source generators.

	if (!GDMonoCache::godot_api_cache_updated) {
		return;
	}

	Dictionary signals_dict;
	// Destructor won't be called from C#, and I don't want to include the GDNative header
	// only for this, so need to call the destructor manually before passing this to C#.
	signals_dict.~Dictionary();

	GDMonoCache::managed_callbacks.ScriptManagerBridge_GetScriptSignalList(this, &signals_dict);

	for (const Variant *s = signals_dict.next(nullptr); s != nullptr; s = signals_dict.next(s)) {
		MethodInfo mi;
		mi.name = *s;

		Array params = signals_dict[*s];

		for (int i = 0; i < params.size(); i++) {
			Dictionary param = params[i];

			Variant::Type param_type = (Variant::Type)(int)param["type"];
			PropertyInfo arg_info = PropertyInfo(param_type, (String)param["name"]);
			if (param_type == Variant::NIL && (bool)param["nil_is_variant"]) {
				arg_info.usage |= PROPERTY_USAGE_NIL_IS_VARIANT;
			}
			mi.arguments.push_back(arg_info);
		}

		r_signals->push_back(mi);
	}
}

bool CSharpScript::inherits_script(const Ref<Script> &p_script) const {
	Ref<CSharpScript> cs = p_script;
	if (cs.is_null()) {
		return false;
	}

	if (!valid || !cs->valid) {
		return false;
	}

	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	return GDMonoCache::managed_callbacks.ScriptManagerBridge_ScriptIsOrInherits(this, cs.ptr());
}

Ref<Script> CSharpScript::get_base_script() const {
	// TODO search in metadata file once we have it, not important any way?
	return Ref<Script>();
}

void CSharpScript::get_script_property_list(List<PropertyInfo> *r_list) const {
	List<PropertyInfo> props;

#ifdef TOOLS_ENABLED
	for (const PropertyInfo &E : exported_members_cache) {
		props.push_back(E);
	}
#else
	for (const KeyValue<StringName, PropertyInfo> &E : member_info) {
		props.push_front(E.value);
	}
#endif // TOOLS_ENABLED

	for (const PropertyInfo &prop : props) {
		r_list->push_back(prop);
	}
}

int CSharpScript::get_member_line(const StringName &p_member) const {
	// TODO omnisharp
	return -1;
}

const Variant CSharpScript::get_rpc_config() const {
	return rpc_config;
}

Error CSharpScript::load_source_code(const String &p_path) {
	Error ferr = read_all_file_utf8(p_path, source);

	ERR_FAIL_COND_V_MSG(ferr != OK, ferr,
			ferr == ERR_INVALID_DATA
					? "Script '" + p_path + "' contains invalid unicode (UTF-8), so it was not loaded."
											" Please ensure that scripts are saved in valid UTF-8 unicode."
					: "Failed to read file: '" + p_path + "'.");

#ifdef TOOLS_ENABLED
	source_changed_cache = true;
#endif

	return OK;
}

void CSharpScript::_clear() {
	tool = false;
	valid = false;
	reload_invalidated = true;
}

CSharpScript::CSharpScript() {
	_clear();

#ifdef DEBUG_ENABLED
	{
		MutexLock lock(CSharpLanguage::get_singleton()->script_instances_mutex);
		CSharpLanguage::get_singleton()->script_list.add(&this->script_list);
	}
#endif
}

CSharpScript::~CSharpScript() {
#ifdef DEBUG_ENABLED
	MutexLock lock(CSharpLanguage::get_singleton()->script_instances_mutex);
	CSharpLanguage::get_singleton()->script_list.remove(&this->script_list);
#endif

	if (GDMonoCache::godot_api_cache_updated) {
		GDMonoCache::managed_callbacks.ScriptManagerBridge_RemoveScriptBridge(this);
	}
}

void CSharpScript::get_members(HashSet<StringName> *p_members) {
#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
	if (p_members) {
		for (const StringName &member_name : exported_members_names) {
			p_members->insert(member_name);
		}
	}
#endif
}

/*************** RESOURCE ***************/

Ref<Resource> ResourceFormatLoaderCSharpScript::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	if (r_error) {
		*r_error = ERR_FILE_CANT_OPEN;
	}

	// TODO ignore anything inside bin/ and obj/ in tools builds?

	CSharpScript *script = memnew(CSharpScript);

	Ref<CSharpScript> scriptres(script);

#if defined(DEBUG_ENABLED) || defined(TOOLS_ENABLED)
	Error err = script->load_source_code(p_path);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Resource>(), "Cannot load C# script file '" + p_path + "'.");
#endif

	script->set_path(p_original_path);

	script->reload();

	if (r_error) {
		*r_error = OK;
	}

	return scriptres;
}

void ResourceFormatLoaderCSharpScript::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("cs");
}

bool ResourceFormatLoaderCSharpScript::handles_type(const String &p_type) const {
	return p_type == "Script" || p_type == CSharpLanguage::get_singleton()->get_type();
}

String ResourceFormatLoaderCSharpScript::get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "cs" ? CSharpLanguage::get_singleton()->get_type() : "";
}

Error ResourceFormatSaverCSharpScript::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<CSharpScript> sqscr = p_resource;
	ERR_FAIL_COND_V(sqscr.is_null(), ERR_INVALID_PARAMETER);

	String source = sqscr->get_source_code();

#ifdef TOOLS_ENABLED
	if (!FileAccess::exists(p_path)) {
		// The file does not yet exist, let's assume the user just created this script. In such
		// cases we need to check whether the solution and csproj were already created or not.
		if (!_create_project_solution_if_needed()) {
			ERR_PRINT("C# project could not be created; cannot add file: '" + p_path + "'.");
		}
	}
#endif

	{
		Error err;
		Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
		ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot save C# script file '" + p_path + "'.");

		file->store_string(source);

		if (file->get_error() != OK && file->get_error() != ERR_FILE_EOF) {
			return ERR_CANT_CREATE;
		}
	}

#ifdef TOOLS_ENABLED
	if (ScriptServer::is_reload_scripts_on_save_enabled()) {
		CSharpLanguage::get_singleton()->reload_tool_script(p_resource, false);
	}
#endif

	return OK;
}

void ResourceFormatSaverCSharpScript::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (Object::cast_to<CSharpScript>(p_resource.ptr())) {
		p_extensions->push_back("cs");
	}
}

bool ResourceFormatSaverCSharpScript::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<CSharpScript>(p_resource.ptr()) != nullptr;
}

CSharpLanguage::StringNameCache::StringNameCache() {
	_script_source = StaticCString::create("script/source");
	on_before_serialize = StaticCString::create("OnBeforeSerialize");
	on_after_deserialize = StaticCString::create("OnAfterDeserialize");
}
