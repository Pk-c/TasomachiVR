#include "umg.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace tasomachivr {
namespace {

using API = uevr::API;

constexpr int kMaxNodes = 140;   // a whole pause menu is large; this keeps the log sane
constexpr int kMaxDepth = 6;

std::string narrow(const std::wstring& w) {
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

std::string class_name_of(API::UObject* object) {
    if (object == nullptr) {
        return "<null>";
    }
    auto* klass = object->get_class();
    if (klass == nullptr) {
        return "<no class>";
    }
    const auto full = narrow(klass->get_full_name());
    // "Class /Script/UMG.TextBlock" -> "TextBlock"
    const auto dot = full.rfind('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

std::string object_name_of(API::UObject* object) {
    if (object == nullptr) {
        return "<null>";
    }
    auto* name = object->get_fname();
    return name != nullptr ? narrow(name->to_string()) : "<unnamed>";
}

API::UObject* deref(API::UObject* owner, const wchar_t* property) {
    if (owner == nullptr) {
        return nullptr;
    }
    auto** slot = owner->get_property_data<API::UObject*>(property);
    return slot != nullptr ? *slot : nullptr;
}

} // namespace

Umg::Blob Umg::make_blob(API::UObject* object, const wchar_t* function) {
    Blob blob{};
    if (object == nullptr) {
        return blob;
    }
    auto* klass = object->get_class();
    if (klass == nullptr) {
        return blob;
    }
    blob.fn = klass->find_function(function);
    if (blob.fn == nullptr) {
        return blob;
    }
    const int32_t size = blob.fn->get_properties_size();
    if (size < 0) {
        return blob;
    }
    blob.bytes.assign(static_cast<size_t>(size), 0);
    blob.ok = true;
    return blob;
}

template <typename T>
bool Umg::put(Blob& blob, const wchar_t* param, const T& value) {
    if (!blob.ok) {
        return false;
    }
    auto* prop = blob.fn->find_property(param);
    if (prop == nullptr) {
        return false;
    }
    const int32_t offset = prop->get_offset();
    if (offset < 0 || offset + static_cast<int32_t>(sizeof(T)) >
                          static_cast<int32_t>(blob.bytes.size())) {
        return false;
    }
    std::memcpy(blob.bytes.data() + offset, &value, sizeof(T));
    return true;
}

template <typename T>
bool Umg::get(const Blob& blob, const wchar_t* param, T& out) {
    if (!blob.ok) {
        return false;
    }
    auto* prop = blob.fn->find_property(param);
    if (prop == nullptr) {
        return false;
    }
    const int32_t offset = prop->get_offset();
    if (offset < 0 || offset + static_cast<int32_t>(sizeof(T)) >
                          static_cast<int32_t>(blob.bytes.size())) {
        return false;
    }
    std::memcpy(&out, blob.bytes.data() + offset, sizeof(T));
    return true;
}

// One level of the tree, then recurse. GetChildrenCount and GetChildAt are
// BlueprintCallable on UPanelWidget, so the walk needs no knowledge of how children are
// stored - which differs between panel types.
void Umg::walk(API::UObject* widget, int depth) {
    if (widget == nullptr || depth > kMaxDepth || m_nodes >= kMaxNodes) {
        return;
    }
    ++m_nodes;

    std::string indent(static_cast<size_t>(depth) * 2, ' ');

    // Visibility matters for grafting: attaching to a collapsed branch would be
    // invisible for reasons that have nothing to do with VR.
    std::string extra;
    if (auto* vis = widget->get_property_data<uint8_t>(L"Visibility")) {
        extra = " visibility=" + std::to_string(static_cast<int>(*vis));
    }

    auto count_call = make_blob(widget, L"GetChildrenCount");
    int32_t count = 0;
    if (count_call.ok) {
        widget->process_event(count_call.fn, count_call.bytes.data());
        get(count_call, L"ReturnValue", count);
    }

    API::get()->log_info("[TasomachiVR] UMG | %s%s (%s)%s%s", indent.c_str(),
                         object_name_of(widget).c_str(), class_name_of(widget).c_str(),
                         extra.c_str(),
                         count_call.ok ? (" children=" + std::to_string(count)).c_str() : "");

    for (int32_t i = 0; i < count && m_nodes < kMaxNodes; ++i) {
        auto child_call = make_blob(widget, L"GetChildAt");
        if (!child_call.ok) {
            break;
        }
        put(child_call, L"Index", i);
        widget->process_event(child_call.fn, child_call.bytes.data());

        API::UObject* child = nullptr;
        if (get(child_call, L"ReturnValue", child) && child != nullptr) {
            walk(child, depth + 1);
        }
    }
}

// Every live UUserWidget, with the class name the engine actually holds. The pause menu
// probe below needs an exact class path, and a path that is merely plausible produces
// silence - so the names come from here instead of from a cooked name table.
void Umg::discover() {
    if (m_discover_attempts >= 6) {
        return;
    }
    ++m_discover_attempts;

    auto* base = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.UserWidget");
    if (base == nullptr) {
        API::get()->log_info("[TasomachiVR] UMG | UserWidget class not found (attempt %d)",
                             m_discover_attempts);
        return;
    }

    const auto widgets = API::UObjectHook::get_objects_by_class(base, false);
    API::get()->log_info("[TasomachiVR] UMG | DISCOVER attempt %d : %d live UserWidget(s)",
                         m_discover_attempts, (int)widgets.size());

    int listed = 0;
    for (auto* widget : widgets) {
        if (listed++ >= 24) {
            break;
        }
        auto* klass = widget->get_class();
        API::get()->log_info("[TasomachiVR] UMG | DISCOVER   %s",
                             klass != nullptr ? narrow(klass->get_full_name()).c_str()
                                              : "<no class>");
    }

    // Once something was found there is nothing left to discover.
    if (!widgets.empty()) {
        m_discover_attempts = 99;
    }
}

void Umg::probe(const wchar_t* class_path) {
    if (std::find(m_probed.begin(), m_probed.end(), class_path) != m_probed.end()) {
        return;
    }

    auto* klass = API::get()->find_uobject<API::UClass>(class_path);
    if (klass == nullptr) {
        // Reported once, then never again: a missing class is a fact worth knowing, and
        // the retry is for the pause menu only existing after the player opens it.
        if (std::find(m_reported.begin(), m_reported.end(), class_path) == m_reported.end()) {
            m_reported.emplace_back(class_path);
            API::get()->log_info("[TasomachiVR] UMG | class not found: %s",
                                 narrow(class_path).c_str());
        }
        return;
    }

    const auto instances = API::UObjectHook::get_objects_by_class(klass, false);
    if (instances.empty()) {
        if (std::find(m_reported.begin(), m_reported.end(), class_path) == m_reported.end()) {
            m_reported.emplace_back(class_path);
            API::get()->log_info("[TasomachiVR] UMG | class found, no live instance yet: %s",
                                 narrow(class_path).c_str());
        }
        return;
    }

    m_probed.emplace_back(class_path);
    API::get()->log_info("[TasomachiVR] UMG | === %s : %d live instance(s) ===",
                         narrow(class_path).c_str(), (int)instances.size());

    for (auto* instance : instances) {
        m_nodes = 0;

        // IsInViewport says whether this instance is the one on screen, which matters
        // when the game has created more than one.
        auto in_viewport = make_blob(instance, L"IsInViewport");
        bool visible = false;
        if (in_viewport.ok) {
            instance->process_event(in_viewport.fn, in_viewport.bytes.data());
            get(in_viewport, L"ReturnValue", visible);
        }

        auto* tree = deref(instance, L"WidgetTree");
        auto* root = deref(tree, L"RootWidget");

        API::get()->log_info("[TasomachiVR] UMG | instance %s in_viewport=%d tree=%s root=%s",
                             object_name_of(instance).c_str(), (int)visible,
                             class_name_of(tree).c_str(), class_name_of(root).c_str());

        walk(root, 1);
        API::get()->log_info("[TasomachiVR] UMG | %d node(s) listed%s", m_nodes,
                             m_nodes >= kMaxNodes ? " (truncated)" : "");
    }
}

} // namespace tasomachivr
