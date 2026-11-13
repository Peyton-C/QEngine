#include "Layout.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Moves an element's coordinates by a template's offset. Only the fields
// that hold positions move; sizes do not.
void offsetElement(json& e, ImVec2 offset) {
    for (const char* key : {"at", "from", "to"}) {
        if (e.contains(key) && e[key].is_array() && e[key].size() >= 2) {
            e[key][0] = e[key][0].get<float>() + offset.x;
            e[key][1] = e[key][1].get<float>() + offset.y;
        }
    }

    if (e.contains("rect") && e["rect"].is_array() && e["rect"].size() >= 2) {
        e["rect"][0] = e["rect"][0].get<float>() + offset.x;
        e["rect"][1] = e["rect"][1].get<float>() + offset.y;
    }
}

void addElement(Layout& out, const json& defaults, json e, int face) {
    Element element;
    element.kind = e.value("kind", "");
    element.face = face;

    // Defaults first, then the element over them, one level deep. Nothing in
    // the format nests deeper than a list of numbers.
    json merged = defaults.contains(element.kind) ? defaults[element.kind] : json::object();

    for (auto& [key, value] : e.items()) {
        merged[key] = value;
    }

    element.id = merged.value("id", "");
    element.p = std::move(merged);
    out.elements.push_back(std::move(element));
}

// Reads one face of the device: its size, body, photo and elements. Elements
// are in the face's own millimetres and moved to where the face sits in the
// whole drawing, and their ids are prefixed so faces cannot collide.
bool addFace(Layout& out, const json& doc, const std::string& name, ImVec2 at, const json& defaults,
             const json& templates, std::string& error) {
    Face face;
    face.name = name;
    const ImVec2 size = vec2(doc, "size", {100, 100});
    face.rect = {at.x, at.y, size.x, size.y};
    face.body = doc.value("body", json::object());

    if (doc.contains("photo")) {
        const json& photo = doc["photo"];
        face.photoFile = photo.value("file", "");

        if (photo.contains("rect") && photo["rect"].size() == 4) {
            const json& r = photo["rect"];
            face.photoRect = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()};
        }
    }

    const int index = int(out.faces.size());
    out.faces.push_back(face);
    const std::string facePrefix = name == "top" ? "" : name + ".";

    // A placed template's "midi_channel" goes to each of its controls that
    // does not name one, as deck 1 and deck 2 send the same notes on their
    // own channels.
    auto place = [&](json e, const std::string& prefix, ImVec2 offset, int channel = -1) {
        offsetElement(e, offset + at);

        if (channel >= 0 && e.contains("midi") && e["midi"].is_object() && !e["midi"].contains("ch")) {
            e["midi"]["ch"] = channel;
        }

        if (e.contains("id")) {
            e["id"] = facePrefix + (prefix.empty() ? "" : prefix + ".") + e["id"].get<std::string>();
        }

        addElement(out, defaults, std::move(e), index);
    };

    for (const json& e : doc.value("elements", json::array())) {
        if (!e.contains("use")) {
            place(e, "", {0, 0});
            continue;
        }

        const std::string use = e["use"].get<std::string>();

        if (!templates.contains(use)) {
            error = "no template named " + use;
            return false;
        }

        // "patch" changes single elements of this copy, by their id inside
        // the template, as deck 2's buttons differ in colour from deck 1's.
        const json patch = e.value("patch", json::object());

        for (json copy : templates[use]) {
            if (copy.contains("id") && patch.contains(copy["id"].get<std::string>())) {
                copy.merge_patch(patch[copy["id"].get<std::string>()]);
            }

            place(std::move(copy), e.value("prefix", ""), vec2(e, "offset"), e.value("midi_channel", -1));
        }
    }

    return true;
}

// Replaces every colour string in a document that a variant's "colors" map
// names. Case is ignored, since hex is written both ways.
void recolour(json& j, const std::unordered_map<std::string, std::string>& map) {
    if (j.is_string()) {
        std::string lower = j.get<std::string>();
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto it = map.find(lower);

        if (it != map.end()) {
            j = it->second;
        }
    } else if (j.is_structured()) {
        for (auto& child : j) {
            recolour(child, map);
        }
    }
}

// Applies a product variant to the document before anything is read from it.
// Top level settings are merge patched, so a variant names only what it
// changes; "colors" swaps colours everywhere; "elements" is applied later, by
// id, once ids are final.
void applyVariant(json& doc, const json& v) {
    for (auto& [key, value] : v.items()) {
        if (key == "colors" || key == "elements" || key == "add") {
            continue;
        }

        if (key == "front" && doc.contains("front")) {
            doc["front"].merge_patch(value);
        } else if (doc.contains(key) && doc[key].is_object() && value.is_object()) {
            doc[key].merge_patch(value);
        } else {
            doc[key] = value;
        }
    }

    if (v.contains("colors")) {
        std::unordered_map<std::string, std::string> map;

        for (auto& [from, to] : v["colors"].items()) {
            std::string lower = from;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            map[lower] = to.get<std::string>();
        }

        json variants = doc.value("variants", json::object());
        doc.erase("variants");
        recolour(doc, map);
        doc["variants"] = std::move(variants);
    }
}

}  // namespace

bool loadLayout(const fs::path& path, Layout& out, std::string& error, const std::string& variant) {
    std::ifstream file(path);

    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }

    json doc;

    try {
        // Comments allowed, so a layout can explain an odd measurement.
        doc = json::parse(file, nullptr, true, true);
    } catch (const json::parse_error& e) {
        error = path.filename().string() + ": " + e.what();
        return false;
    }

    Layout layout;
    layout.path = path;

    if (doc.contains("variants")) {
        for (auto& [code, v] : doc["variants"].items()) {
            layout.variants.push_back(code);
        }
    }

    json variantDoc = json::object();

    if (!variant.empty()) {
        if (!doc.contains("variants") || !doc["variants"].contains(variant)) {
            error = path.filename().string() + ": no variant " + variant;
            return false;
        }

        variantDoc = doc["variants"][variant];
        applyVariant(doc, variantDoc);
        layout.variant = variant;
    }

    layout.name = doc.value("name", path.stem().string());

    std::error_code ec;
    layout.mtime = fs::last_write_time(path, ec);

    try {
        // A variant is one product; the base serves the rest.
        if (!variant.empty()) {
            layout.products = {variant};
        } else if (doc.contains("products")) {
            layout.products = doc["products"].get<std::vector<std::string>>();
        }

        if (doc.contains("midi")) {
            layout.midiChannel = doc["midi"].value("channel", layout.midiChannel);
        }

        if (doc.contains("fonts")) {
            layout.labelSize = num(doc["fonts"], "label", layout.labelSize);
            layout.textScale = num(doc["fonts"], "scale", layout.textScale);
        }

        const json defaults = doc.value("defaults", json::object());
        const json templates = doc.value("templates", json::object());

        if (!addFace(layout, doc, "top", {0, 0}, defaults, templates, error)) {
            error = path.filename().string() + ": " + error;
            return false;
        }

        const ImVec4 top = layout.faces[0].rect;
        layout.size = {top.z, top.w};

        // The front panel goes under the top, centred, as if the device were
        // tipped towards the reader.
        if (doc.contains("front")) {
            const json& front = doc["front"];
            const ImVec2 size = vec2(front, "size", {top.z, 40});
            const ImVec2 at = {(top.z - size.x) * 0.5f, top.w + num(front, "gap", 12.0f)};

            if (!addFace(layout, front, "front", at, defaults, templates, error)) {
                error = path.filename().string() + ": " + error;
                return false;
            }

            layout.size = {std::max(top.z, size.x), at.y + size.y};
        }

        // Elements only this product has, in the top's millimetres.
        for (const json& e : variantDoc.value("add", json::array())) {
            addElement(layout, defaults, e, 0);
        }

        // Per element changes, by final id: "deck1.sync", "front.phones".
        // {"hidden": true} removes one.
        if (variantDoc.contains("elements")) {
            for (Element& e : layout.elements) {
                if (!e.id.empty() && variantDoc["elements"].contains(e.id)) {
                    e.p.merge_patch(variantDoc["elements"][e.id]);
                }
            }
        }
    } catch (const json::exception& e) {
        error = path.filename().string() + ": " + e.what();
        return false;
    }

    out = std::move(layout);
    return true;
}

std::vector<fs::path> findLayouts(const fs::path& dir) {
    std::vector<fs::path> found;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            found.push_back(entry.path());
        }
    }

    std::sort(found.begin(), found.end());
    return found;
}

float num(const json& p, const char* key, float fallback) {
    auto it = p.find(key);
    return (it != p.end() && it->is_number()) ? it->get<float>() : fallback;
}

bool flag(const json& p, const char* key, bool fallback) {
    auto it = p.find(key);
    return (it != p.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

std::string str(const json& p, const char* key, const char* fallback) {
    auto it = p.find(key);
    return (it != p.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

ImVec2 vec2(const json& p, const char* key, ImVec2 fallback) {
    auto it = p.find(key);

    if (it == p.end()) {
        return fallback;
    }

    // A bare number is a square or a circle's diameter.
    if (it->is_number()) {
        const float v = it->get<float>();
        return {v, v};
    }

    if (it->is_array() && it->size() >= 2) {
        return {(*it)[0].get<float>(), (*it)[1].get<float>()};
    }

    return fallback;
}

ImVec4 rect(const json& p, const char* key) {
    auto it = p.find(key);

    if (it == p.end() || !it->is_array() || it->size() < 4) {
        return {0, 0, 0, 0};
    }

    return {(*it)[0].get<float>(), (*it)[1].get<float>(),
            (*it)[2].get<float>(), (*it)[3].get<float>()};
}

ImVec4 parseColour(const std::string& hex, ImVec4 fallback) {
    if (hex.size() != 7 && hex.size() != 9) {
        return fallback;
    }

    if (hex[0] != '#') {
        return fallback;
    }

    unsigned value = 0;

    try {
        value = static_cast<unsigned>(std::stoul(hex.substr(1), nullptr, 16));
    } catch (...) {
        return fallback;
    }

    if (hex.size() == 7) {
        value = (value << 8) | 0xFF;
    }

    return {((value >> 24) & 0xFF) / 255.0f, ((value >> 16) & 0xFF) / 255.0f,
            ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f};
}

bool hasColour(const json& p, const char* key) {
    auto it = p.find(key);
    return it != p.end() && it->is_string();
}

ImVec4 colour(const json& p, const char* key, ImVec4 fallback) {
    auto it = p.find(key);
    return (it != p.end() && it->is_string()) ? parseColour(it->get<std::string>(), fallback)
                                              : fallback;
}
