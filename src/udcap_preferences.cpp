#include "udcap_preferences.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace opencap {
namespace {
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;
using Values = std::map<std::string, std::string, std::less<>>;
using Path = std::filesystem::path;

class Apartment {
public:
    Apartment() : result_(RoInitialize(RO_INIT_SINGLETHREADED)) {}
    ~Apartment() { if (SUCCEEDED(result_)) RoUninitialize(); }
private:
    HRESULT result_;
};

std::optional<JsonObject> readJson(const Path& path) {
    std::error_code error;
    if (path.empty() || std::filesystem::file_size(path, error) > 2 * 1024 * 1024 || error) return std::nullopt;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::string text{std::istreambuf_iterator<char>(in), {}};
        if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
        JsonObject object{nullptr};
        if (JsonObject::TryParse(winrt::to_hstring(text), object)) return object;
    } catch (const winrt::hresult_error&) {}
    return std::nullopt;
}

IJsonValue get(const JsonObject& object, const std::string& key) {
    if (!object) return nullptr;
    const auto name = winrt::to_hstring(key);
    return object.HasKey(name) ? object.GetNamedValue(name) : nullptr;
}

JsonObject group(const JsonObject& object, const std::string& key) {
    const auto value = get(object, key);
    return value && value.ValueType() == JsonValueType::Object ? value.GetObject() : nullptr;
}

std::optional<double> parseNumber(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;
    if (text.front() == '+') text.remove_prefix(1);
    double value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) return std::nullopt;
    return value;
}

const std::string& setting(const Values& values, const std::string& key) {
    static const std::string empty;
    const auto found = values.find(key);
    return found == values.end() ? empty : found->second;
}

std::optional<bool> parseBool(std::string_view value) {
    std::string lower;
    for (const char c : value) if (c != ' ' && c != '\t') lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c);
    if (lower == "true" || lower == "1") return true;
    if (lower == "false" || lower == "0") return false;
    return std::nullopt;
}

std::optional<double> number(const JsonObject& obj, const std::string& key) {
    const auto value = get(obj, key);
    if (!value) return std::nullopt;
    if (value.ValueType() == JsonValueType::Number) return value.GetNumber();
    if (value.ValueType() == JsonValueType::String) return parseNumber(winrt::to_string(value.GetString()));
    return std::nullopt;
}

std::optional<bool> boolean(const JsonObject& obj, const std::string& key) {
    const auto value = get(obj, key);
    if (!value) return std::nullopt;
    if (value.ValueType() == JsonValueType::Boolean) return value.GetBoolean();
    if (value.ValueType() == JsonValueType::String) return parseBool(winrt::to_string(value.GetString()));
    return std::nullopt;
}

bool within(double value, double lo, double hi) { return value >= lo && value <= hi; }

Values readFactory(const Path& path) {
    Values result;
    winrt::com_ptr<IStream> input;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, input.put())))
        return result;
    winrt::com_ptr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reader.put_void(), nullptr))) return result;
    if (FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetInput(input.get()))) return result;
    bool inSettings = false;
    XmlNodeType type{};
    while (reader->Read(&type) == S_OK) {
        if (type != XmlNodeType_Element && type != XmlNodeType_EndElement) continue;
        const wchar_t* name{};
        UINT nameLength{};
        if (FAILED(reader->GetLocalName(&name, &nameLength)) || !name) continue;
        const std::wstring_view tag(name, nameLength);
        if (tag == L"appSettings") inSettings = type == XmlNodeType_Element;
        if (!inSettings || type != XmlNodeType_Element || tag != L"add") continue;
        const wchar_t* value{};
        UINT valueLength{};
        if (reader->MoveToAttributeByName(L"key", nullptr) != S_OK ||
            FAILED(reader->GetValue(&value, &valueLength)) || !value) continue;
        auto key = winrt::to_string(std::wstring_view(value, valueLength));
        if (reader->MoveToAttributeByName(L"value", nullptr) != S_OK ||
            FAILED(reader->GetValue(&value, &valueLength)) || !value) continue;
        result.insert_or_assign(std::move(key), winrt::to_string(std::wstring_view(value, valueLength)));
        reader->MoveToElement();
    }
    return result;
}

void overlaySettings(Values& values, const JsonObject& saved) {
    if (!saved) return;
    for (const auto& entry : saved) {
        const auto value = entry.Value();
        const auto key = winrt::to_string(entry.Key());
        switch (value.ValueType()) {
        case JsonValueType::String: if (!value.GetString().empty()) values[key] = winrt::to_string(value.GetString()); break;
        case JsonValueType::Boolean: values[key] = value.GetBoolean() ? "true" : "false"; break;
        case JsonValueType::Number: values[key] = winrt::to_string(value.Stringify()); break;
        default: break;
        }
    }
}

void applyPose(const JsonObject& source, std::array<HandSettings, handCount>& hands, bool useTrackerIds) {
    const auto pose = group(source, "pose_settings");
    const auto driver = group(source, "driver_UDCAP");
    if (!pose && !driver) return;
    const auto globalFeedback = boolean(driver, "feedback_enabled");
    for (std::size_t i = 0; i < handCount; ++i) {
        auto& h = hands[i];
        const std::string side = i ? "right" : "left";
        const std::array<std::pair<const char*, float*>, 6> fields{{
            {"x_offset_position", &h.position.x}, {"y_offset_position", &h.position.y},
            {"z_offset_position", &h.position.z}, {"x_offset_degrees", &h.rotation.x},
            {"y_offset_degrees", &h.rotation.y}, {"z_offset_degrees", &h.rotation.z}}};
        for (std::size_t f = 0; f < fields.size(); ++f) {
            const double limit = f < 3 ? 1 : 360;
            if (const auto value = number(pose, side + "_" + fields[f].first); value && within(*value, -limit, limit))
                *fields[f].second = static_cast<float>(*value);
        }
        if (const auto offset = number(pose, "pose_time_offset"); offset && within(*offset, -.25, .25))
            h.poseTimeOffset = static_cast<float>(*offset);
        if (const auto enabled = boolean(driver, side + "_enabled")) h.enabled = *enabled;
        if (globalFeedback) h.haptics = *globalFeedback;
        if (const auto bones = boolean(driver, "skeleton_only")) h.skeletonOnly = *bones;
        if (useTrackerIds && boolean(pose, "controller_override").value_or(false)) {
            if (const auto index = number(pose, "controller_override_" + side);
                index && within(*index, 1, 63) && std::trunc(*index) == *index)
                h.tracker = static_cast<std::uint32_t>(*index);
        }
    }
}

void applyCustomPose(const Values& values, std::array<HandSettings, handCount>& hands) {
    if (const auto preset = parseNumber(setting(values, "VRSpaceSelectedIndex"));
        preset && within(*preset, 0, trackingPresets.size()-1) && std::trunc(*preset) == *preset) {
        applyTrackingPreset(hands, static_cast<std::size_t>(*preset));
        return;
    }
    if (!parseBool(setting(values, "VRSpaceCustom")).value_or(false)) return;
    auto csv = std::string_view(setting(values, "VRSpaceCustomValueStr"));
    std::array<float, 12> data{};
    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto delimiter = csv.find(',');
        const auto value = parseNumber(csv.substr(0, delimiter));
        const auto limit = i % 6 < 3 ? 1.0 : 360.0;
        if (!value || !within(*value, -limit, limit)) return;
        data[i] = static_cast<float>(*value);
        if (delimiter == std::string_view::npos && i + 1 < data.size()) return;
        if (delimiter != std::string_view::npos) csv.remove_prefix(delimiter + 1);
    }
    // Desktop stores right first, with the editor's Z position and Y rotation signs.
    for (std::size_t h = 0; h < handCount; ++h) {
        const auto v = (1-h) * 6;
        hands[h].position = {data[v], data[v+1], -data[v+2]};
        hands[h].rotation = {data[v+3], -data[v+4], data[v+5]};
    }
}

constexpr std::array<double, 21> curveMin{
    -60,-60,-65,-20, -80,-100,-80,0, -80,-100,-80,0,
    -80,-100,-80,0, -74,-92,-75,0,0};
constexpr std::array<double, 21> curveMax{
    0,0,0,15, 0,0,0,25, 0,0,0,0, 0,0,0,15, 0,0,0,35,47};

ReceiverPreferences receiverSettings(const Values& values, const std::string& side) {
    ReceiverPreferences result;
    auto read = [&values, &side](std::string_view key) { return parseNumber(setting(values, side + std::string(key))); };
    const auto numberOr = [&read](std::string_view key, double value, double lo, double hi) {
        const auto x = read(key);
        return x && within(*x, lo, hi) ? *x : value;
    };
    const std::array<std::array<const char*, 6>, 2> axes{{
        {"Rocky_X_Min", "Rocky_X_Center", "Rocky_X_Max", "xMinData", "xCenterData", "xMaxData"},
        {"Rocky_Y_Min", "Rocky_Y_Center", "Rocky_Y_Max", "yMinData", "yCenterData", "yMaxData"}}};
    for (const auto& axis : axes) {
        const auto lo = read(axis[0]), mid = read(axis[1]), hi = read(axis[2]);
        if (lo && mid && hi && within(*lo, 0, 4095) && within(*hi, 0, 4095) && *lo < *mid && *mid < *hi) {
            result.controller[axis[3]] = *lo;
            result.controller[axis[4]] = *mid;
            result.controller[axis[5]] = *hi;
        }
    }
    if (const auto dead = read("DeadZone"); dead && within(*dead, 0, 1)) result.controller["deadZone"] = *dead;
    struct Threshold { const char* low; const char* high; const char* lowOut; const char* highOut; };
    constexpr std::array thresholds{
        Threshold{"TriggerValue_Min", "TriggerValue", "triggerMin", "triggerMax"},
        Threshold{"GrabValue", "GrabValue_Max", "gripMin", "gripMax"},
        Threshold{"TrackpadValue", "TrackpadValue_Max", "trackpadMin", "trackpadMax"}};
    for (const auto& t : thresholds) {
        const auto low = read(t.low), high = read(t.high);
        if (low && high && within(*low, 0, 100) && within(*high, 0, 100) && *low <= *high) {
            result.controller[t.lowOut] = *low / 100.0;
            result.controller[t.highOut] = *high / 100.0;
        }
    }

    constexpr std::array labels{
        "Thumb3_Pitch", "Thumb2_Pitch", "Thumb1_Pitch", "Thumb1_Yaw", "Index3_Pitch", "Index2_Pitch", "Index1_Pitch", "Index1_Yaw",
        "Middle3_Pitch", "Middle2_Pitch", "Middle1_Pitch", "Middle1_Yaw", "Ring3_Pitch", "Ring2_Pitch", "Ring1_Pitch",
        "Ring1_Yaw", "Pinky3_Pitch", "Pinky2_Pitch", "Pinky1_Pitch", "Pinky1_Yaw", "Thumb1_Roll"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const std::string name = labels[i];
        auto& c = result.curves[i];
        c = {curveMin[i], curveMax[i], .5, 0};
        if (i == 11) {
            c.offset = numberOr("Output_Middle1_Yaw", 0, -45, 45);
        } else {
            if (name.ends_with("_Pitch")) {
                const auto gain = numberOr("Coefficient_" + name, 1, 0, 3);
                c.min = (std::max)(-180.0, c.min * gain);
                c.max = (std::min)(180.0, c.max * gain);
                c.offset = numberOr("Offset_" + name, 0, -45, 45);
            } else {
                const auto lo = numberOr("Min_Output_" + name, c.min, -180, 180);
                const auto hi = numberOr("Max_Output_" + name, c.max, -180, 180);
                if (lo <= hi) { c.min = lo; c.max = hi; }
            }
            c.balance = numberOr("Balance_" + name, .5, 0, 1);
        }
        result.hasCurves = result.hasCurves || read("Balance_" + name).has_value()
            || read("Coefficient_" + name).has_value() || read("Output_" + name).has_value();
    }
    return result;
}

void streamValue(const Values& values, const std::string& prefix, std::string& ip, unsigned& port) {
    const auto& host = setting(values, prefix + "_IP");
    IN_ADDR address{};
    if (!host.empty() && InetPtonA(AF_INET, host.c_str(), &address) != 1) return;
    if (!host.empty()) ip = host;
    if (!parseBool(setting(values, prefix + "_State")).value_or(false)) return;
    if (const auto p = parseNumber(setting(values, prefix + "_PORT")); p && within(*p, 1, 65535) && std::trunc(*p) == *p)
        port = static_cast<unsigned>(*p);
}
}

UdcapSources findUdcapSources(const Path& localAppData) {
    std::wstring dir(32768, L'\0');
    const auto size = GetEnvironmentVariableW(L"ProgramFiles", dir.data(), static_cast<DWORD>(dir.size()));
    const auto install = (size && size < dir.size() ? Path(dir.substr(0, size)) : Path(L"C:\\Program Files")) / L"UdcapDriver";
    return {install / L"UdcapDriver.dll.config", localAppData / L"UdcapDriver" / L"config.json",
        install / L"udcap" / L"resources" / L"settings" / L"default.vrsettings",
        localAppData / L"openvr" / L"openvrpaths.vrpath"};
}

UdcapPreferences readUdcapPreferences(const UdcapSources& sources) {
    UdcapPreferences result;
    thread_local Apartment apartment;
    auto values = readFactory(sources.desktopDefaults);
    if (const auto document = readJson(sources.desktopSettings)) {
        const auto saved = group(*document, "Settings");
        result.saved = saved && saved.Size() != 0;
        overlaySettings(values, saved);
    }
    result.found = !values.empty();
    if (const auto steam = readJson(sources.steamvrDefaults)) {
        result.found = true;
        applyPose(*steam, result.hands, false);
    }
    for (std::size_t i = 0; i < handCount; ++i) {
        auto& h = result.hands[i];
        const std::string side = i ? "R_" : "L_";
        h.highPriority = parseBool(setting(values, "Controller_Priority")).value_or(h.highPriority);
        h.haptics = parseBool(setting(values, side + "Vibartion_Enable")).value_or(h.haptics);
        h.triggerEnabled = parseBool(setting(values, side + "TriggerOn")).value_or(h.triggerEnabled);
        h.gripEnabled = parseBool(setting(values, side + "GrabOn")).value_or(h.gripEnabled);
        h.padEnabled = parseBool(setting(values, side + "TrackpadOn")).value_or(h.padEnabled);
        h.thumbEnabled = parseBool(setting(values, side + "ThumbOn")).value_or(h.thumbEnabled);
        result.receiver[i] = receiverSettings(values, side);
    }
    streamValue(values, "VMC", result.stream.address, result.stream.vmcPort);
    streamValue(values, "OSC", result.stream.oscAddress, result.stream.oscPort);
    result.quickStick = parseBool(setting(values, "Controller_Module")).value_or(false);
    applyCustomPose(values, result.hands);
    if (const auto registry = readJson(sources.steamvrRegistry)) {
        const auto config = get(*registry, "config");
        if (config && config.ValueType() == JsonValueType::Array) {
            for (const auto& entry : config.GetArray()) {
                if (entry.ValueType() != JsonValueType::String) continue;
                if (const auto steam = readJson(Path(entry.GetString().c_str()) / L"steamvr.vrsettings");
                    steam && group(*steam, "driver_UDCAP")) {
                    result.found = true;
                    applyPose(*steam, result.hands, true);
                    break;
                }
            }
        }
    }
    return result;
}

void applyUdcapPreferences(const UdcapPreferences& source,
                          std::array<HandSettings, handCount>& hands, StreamSettings& stream) {
    if (!source.found) return;
    for (std::size_t i = 0; i < handCount; ++i) {
        const auto buttonMap = hands[i].buttonMap;
        const auto joystick = hands[i].joystick;
        const auto serial = hands[i].trackerSerial;
        hands[i] = source.hands[i];
        hands[i].buttonMap = buttonMap;
        hands[i].joystick = joystick;
        if (hands[i].tracker == noTracker) hands[i].trackerSerial = serial;
    }
    stream = source.stream;
}
}
