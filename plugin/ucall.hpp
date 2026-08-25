// TasomachiVR - calling UFunctions and building their parameter blobs.
//
// Header-only, because these are small and every user wants them inlined.
//
// Two lessons are baked in here, both paid for in test runs:
//
//   * Arguments are addressed BY POSITION, never by name. UHT does not keep argument
//     names stable and the lookup is case sensitive: Conv_StringToText's first argument
//     is "inString" here, not "InString", and a name-based write silently failed.
//     "ReturnValue" is the one name that is stable, so it is the only one used.
//
//   * Strings handed to the engine are allocated with the engine's FMalloc. A parameter
//     blob's FString is destroyed by the engine when ProcessEvent is done with it, and
//     freeing a CRT pointer there produces exactly the "FMallocBinned2 realloc an
//     unrecognized block" assert this game already crashed on once.
#pragma once

#include <uevr/API.hpp>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tasomachivr::ucall {

using API = uevr::API;

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    }
    return out;
}

inline std::string object_name(API::UObject* object) {
    if (object == nullptr) {
        return "<null>";
    }
    auto* name = object->get_fname();
    return name != nullptr ? narrow(name->to_string()) : "<unnamed>";
}

inline std::string class_name(API::UObject* object) {
    if (object == nullptr) {
        return "<null>";
    }
    auto* klass = object->get_class();
    if (klass == nullptr) {
        return "<no class>";
    }
    const auto full = narrow(klass->get_full_name());
    const auto dot = full.rfind('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

inline API::UObject* property_object(API::UObject* owner, const wchar_t* name) {
    if (owner == nullptr) {
        return nullptr;
    }
    auto** slot = owner->get_property_data<API::UObject*>(name);
    return slot != nullptr ? *slot : nullptr;
}

// UE4's FString: a heap pointer, a count that includes the terminator, and a capacity.
struct EngineString {
    wchar_t* data{nullptr};
    int32_t num{0};
    int32_t max{0};
};

inline EngineString engine_string(const wchar_t* text) {
    EngineString out{};
    auto* malloc = API::FMalloc::get();
    if (malloc == nullptr || text == nullptr) {
        return out;
    }

    const size_t len = wcslen(text);
    const auto bytes = static_cast<uint32_t>((len + 1) * sizeof(wchar_t));
    out.data = static_cast<wchar_t*>(malloc->malloc(bytes, 0));
    if (out.data == nullptr) {
        return out;
    }
    std::memcpy(out.data, text, bytes);
    out.num = static_cast<int32_t>(len) + 1;
    out.max = out.num;
    return out;
}

// A resolved UFunction plus a zeroed parameter blob of the right size.
struct Call {
    API::UFunction* fn{nullptr};
    std::vector<uint8_t> bytes;
    bool ok{false};

    explicit Call(API::UObject* object, const wchar_t* function) {
        if (object == nullptr) {
            return;
        }
        auto* klass = object->get_class();
        if (klass == nullptr) {
            return;
        }
        fn = klass->find_function(function);
        if (fn == nullptr) {
            return;
        }
        const int32_t size = fn->get_properties_size();
        if (size < 0) {
            return;
        }
        bytes.assign(static_cast<size_t>(size), 0);
        ok = true;
    }
};

// The first child property that is not the return value.
inline API::FProperty* first_param(API::UFunction* fn) {
    if (fn == nullptr) {
        return nullptr;
    }
    for (auto* field = fn->get_child_properties(); field != nullptr; field = field->get_next()) {
        if (field->get_fname() != nullptr &&
            narrow(field->get_fname()->to_string()) == "ReturnValue") {
            continue;
        }
        return reinterpret_cast<API::FProperty*>(field);
    }
    return nullptr;
}

// The nth argument, skipping the return value. Positional, for the same reason.
inline API::FProperty* param_at(API::UFunction* fn, int index) {
    if (fn == nullptr) {
        return nullptr;
    }
    int seen = 0;
    for (auto* field = fn->get_child_properties(); field != nullptr; field = field->get_next()) {
        if (field->get_fname() != nullptr &&
            narrow(field->get_fname()->to_string()) == "ReturnValue") {
            continue;
        }
        if (seen++ == index) {
            return reinterpret_cast<API::FProperty*>(field);
        }
    }
    return nullptr;
}

inline API::FProperty* return_param(API::UFunction* fn) {
    if (fn == nullptr) {
        return nullptr;
    }
    for (auto* field = fn->get_child_properties(); field != nullptr; field = field->get_next()) {
        if (field->get_fname() != nullptr &&
            narrow(field->get_fname()->to_string()) == "ReturnValue") {
            return reinterpret_cast<API::FProperty*>(field);
        }
    }
    return nullptr;
}

template <typename T>
bool write_at(Call& call, API::FProperty* prop, const T& value) {
    if (!call.ok || prop == nullptr) {
        return false;
    }
    const int32_t offset = prop->get_offset();
    if (offset < 0 ||
        offset + static_cast<int32_t>(sizeof(T)) > static_cast<int32_t>(call.bytes.size())) {
        return false;
    }
    std::memcpy(call.bytes.data() + offset, &value, sizeof(T));
    return true;
}

template <typename T>
bool put(Call& call, int index, const T& value) {
    return write_at(call, param_at(call.fn, index), value);
}

template <typename T>
bool result(const Call& call, T& out) {
    auto* prop = return_param(call.fn);
    if (!call.ok || prop == nullptr) {
        return false;
    }
    const int32_t offset = prop->get_offset();
    if (offset < 0 ||
        offset + static_cast<int32_t>(sizeof(T)) > static_cast<int32_t>(call.bytes.size())) {
        return false;
    }
    std::memcpy(&out, call.bytes.data() + offset, sizeof(T));
    return true;
}

// --- the handful of engine calls the menu actually makes ------------------------

inline API::UObject* spawn(const wchar_t* class_path, API::UObject* outer) {
    auto* klass = API::get()->find_uobject<API::UClass>(class_path);
    if (klass == nullptr || outer == nullptr) {
        return nullptr;
    }
    return API::get()->spawn_object(klass, outer);
}

// Returns the slot AddChild produced. Ignoring it was what made a runtime-built page
// visible but not clickable: an unconfigured CanvasPanelSlot has no geometry, the
// CanvasPanel does not clip by default so the content still draws, and hit testing is
// bounded by the geometry - so nothing receives the cursor.
inline API::UObject* add_child(API::UObject* panel, API::UObject* child) {
    Call call{panel, L"AddChild"};
    if (!call.ok || !put(call, 0, child)) {
        return nullptr;
    }
    panel->process_event(call.fn, call.bytes.data());
    API::UObject* slot = nullptr;
    result(call, slot);
    return slot;
}

// One- and two-argument calls, which is all the slot configuration needs.
template <typename T>
inline bool call_one(API::UObject* object, const wchar_t* function, const T& value) {
    Call call{object, function};
    if (!call.ok || !put(call, 0, value)) {
        return false;
    }
    object->process_event(call.fn, call.bytes.data());
    return true;
}

template <typename A, typename B>
inline bool call_two(API::UObject* object, const wchar_t* function, const A& a, const B& b) {
    Call call{object, function};
    if (!call.ok || !put(call, 0, a) || !put(call, 1, b)) {
        return false;
    }
    object->process_event(call.fn, call.bytes.data());
    return true;
}

// UWidget::SetRenderOpacity - a real alpha on the widget and everything under it, which is
// what makes a fade possible at all. Toggling visibility can only pop.
inline bool set_opacity(API::UObject* widget, float alpha) {
    Call call{widget, L"SetRenderOpacity"};
    if (!call.ok || !put(call, 0, alpha)) {
        return false;
    }
    widget->process_event(call.fn, call.bytes.data());
    return true;
}

inline API::UObject* child_at(API::UObject* panel, int32_t index) {
    Call call{panel, L"GetChildAt"};
    if (!call.ok || !put(call, 0, index)) {
        return nullptr;
    }
    panel->process_event(call.fn, call.bytes.data());
    API::UObject* child = nullptr;
    result(call, child);
    return child;
}

// ESlateVisibility: 0 Visible, 1 Collapsed, 2 Hidden, 3 HitTestInvisible,
// 4 SelfHitTestInvisible. The game's own pages toggle between 2 and 4, so the menu
// follows that convention rather than inventing one.
inline bool set_visibility(API::UObject* widget, uint8_t visibility) {
    Call call{widget, L"SetVisibility"};
    if (!call.ok || !put(call, 0, visibility)) {
        return false;
    }
    widget->process_event(call.fn, call.bytes.data());
    return true;
}

inline bool is_pressed(API::UObject* button) {
    Call call{button, L"IsPressed"};
    if (!call.ok) {
        return false;
    }
    button->process_event(call.fn, call.bytes.data());
    bool pressed = false;
    result(call, pressed);
    return pressed;
}

// Copies a struct property from one object into a one-argument setter on another,
// without knowing anything about the struct's layout: the argument's size is the rest of
// the parameter blob, which is exact for a void function taking a single struct.
//
// This is how the menu borrows the game's own look - FButtonStyle, FSlateFontInfo and
// FSlateColor are all read off an existing widget and handed straight to the setter.
// The copy duplicates the object pointers inside brushes without adding a reference;
// safe here because source and destination live and die in the same widget tree.
inline bool copy_struct_arg(API::UObject* source, const wchar_t* property,
                           API::UObject* target, const wchar_t* setter) {
    if (source == nullptr || target == nullptr) {
        return false;
    }
    Call call{target, setter};
    auto* param = first_param(call.fn);
    if (!call.ok || param == nullptr) {
        return false;
    }
    const int32_t offset = param->get_offset();
    const int32_t size = static_cast<int32_t>(call.bytes.size()) - offset;
    auto* bytes = source->get_property_data<uint8_t>(property);
    if (bytes == nullptr || offset < 0 || size <= 0) {
        return false;
    }
    std::memcpy(call.bytes.data() + offset, bytes, static_cast<size_t>(size));
    target->process_event(call.fn, call.bytes.data());
    return true;
}

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

// GetSocketTransform(InSocketName, TransformSpace) -> FTransform, space 0 being world.
// FTransform is a quaternion then a translation, so the translation is read by offset
// from the return value rather than by modelling the whole struct.
// K2_GetComponentLocation is on SceneComponent and returns an FVector in world space.
inline bool component_location(API::UObject* component, Vec3& out) {
    Call call{component, L"K2_GetComponentLocation"};
    if (!call.ok) {
        return false;
    }
    component->process_event(call.fn, call.bytes.data());
    return result(call, out);
}

inline bool socket_location(API::UObject* mesh, const wchar_t* bone, Vec3& out) {
    Call call{mesh, L"GetSocketTransform"};
    if (!call.ok) {
        return false;
    }
    const API::FName name{bone};
    if (!put(call, 0, name) || !put(call, 1, uint8_t{0})) {
        return false;
    }
    mesh->process_event(call.fn, call.bytes.data());

    auto* ret = return_param(call.fn);
    if (ret == nullptr) {
        return false;
    }
    const int32_t offset = ret->get_offset() + 16; // past FQuat Rotation
    if (offset + static_cast<int32_t>(sizeof(Vec3)) > static_cast<int32_t>(call.bytes.size())) {
        return false;
    }
    std::memcpy(&out, call.bytes.data() + offset, sizeof(Vec3));
    return true;
}

// Builds an FText from a string and hands it to a widget's SetText.
//
// The FText is copied out of Conv_StringToText's blob and into SetText's by offset and
// size, so nothing here needs to know how FText is laid out. Note the copy does not
// touch the refcount, which leaks one FText per call - acceptable for a label that
// changes when a value changes, and the reason values are not refreshed every frame.
inline bool set_text(API::UObject* widget, const wchar_t* text) {
    auto* library = API::get()->find_uobject<API::UObject>(
        L"KismetTextLibrary /Script/Engine.Default__KismetTextLibrary");
    if (library == nullptr || widget == nullptr) {
        return false;
    }

    Call conv{library, L"Conv_StringToText"};
    if (!conv.ok) {
        return false;
    }
    const auto string = engine_string(text);
    if (string.data == nullptr || !put(conv, 0, string)) {
        return false;
    }
    library->process_event(conv.fn, conv.bytes.data());

    auto* ret = return_param(conv.fn);
    if (ret == nullptr) {
        return false;
    }
    const int32_t ret_offset = ret->get_offset();
    const int32_t ret_size = static_cast<int32_t>(conv.bytes.size()) - ret_offset;

    Call set{widget, L"SetText"};
    auto* in_text = first_param(set.fn);
    if (!set.ok || in_text == nullptr ||
        in_text->get_offset() + ret_size > static_cast<int32_t>(set.bytes.size())) {
        return false;
    }
    std::memcpy(set.bytes.data() + in_text->get_offset(), conv.bytes.data() + ret_offset,
                static_cast<size_t>(ret_size));
    widget->process_event(set.fn, set.bytes.data());
    return true;
}

} // namespace tasomachivr::ucall
