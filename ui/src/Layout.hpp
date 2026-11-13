#pragma once

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

// A device's front panel as data. Positions and sizes are millimetres from
// the top left of the faceplate, so one set of drawing code serves every
// device and a 12 mm button is the same size on all of them.
//
// A layout file has:
//   size       the faceplate, [width, height]
//   body       its colour, corner radius and edge
//   photo      optional, a product photo and the pixel rect of the faceplate
//              in it, used by the Photo and Trace views
//   front      optional, the front panel, with its own size, body, photo,
//              elements and a gap above it; defaults and templates are shared
//   variants   optional, product codes that differ from the base, each
//              merging over the settings above, swapping colours with
//              "colors", patching or hiding single elements by id with
//              "elements", and adding its own with "add".
//              Each is listed as its own device.
//   defaults   properties every element of a kind starts from, so a device's
//              button style is written once
//   templates  named lists of elements, placed with {"use", "prefix",
//              "offset"}; a two deck unit draws its deck once
//   elements   drawn in order, later ones on top
//   midi       optional, {"channel": N}, the channel for controls that name
//              none; see Midi.hpp for a control's own "midi" field
//
// Unknown properties are kept, so the format can grow without the loader
// knowing about every field.

struct Element {
    std::string id;    // empty for decoration
    int face = 0;      // index into Layout::faces
    std::string kind;  // button, pad, knob, encoder, fader, jog, lcd, ...
    nlohmann::json p;  // defaults for the kind, then the element's own fields
};

// One side of the device. The top is always faces[0]; a layout's optional
// "front" adds a second, drawn below it.
struct Face {
    std::string name;
    ImVec4 rect = {0, 0, 100, 100};  // x, y, w, h in the whole drawing, mm
    nlohmann::json body;
    std::string photoFile;
    ImVec4 photoRect = {0, 0, 0, 0};  // x0, y0, x1, y1 in photo pixels
};

struct Layout {
    std::string name;
    std::vector<std::string> products;
    ImVec2 size = {100, 100};  // every face together
    float labelSize = 2.4f;    // cap height of control legends, mm
    float textScale = 1.0f;    // for a maker whose printing runs wider than ours
    int midiChannel = 0;       // for controls whose "midi" names no channel, 0 to 15

    std::vector<Face> faces;
    std::vector<Element> elements;

    std::string variant;                // the product code loaded, or empty for the base
    std::vector<std::string> variants;  // every variant the file defines

    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
};

// Loads the base layout, or with `variant` one of its product variants.
bool loadLayout(const std::filesystem::path& path, Layout& out, std::string& error,
                const std::string& variant = "");

// Every *.json in dir, sorted by file name.
std::vector<std::filesystem::path> findLayouts(const std::filesystem::path& dir);

// Accessors that take a default, so drawing code never has to check whether
// a field was given.
float num(const nlohmann::json& p, const char* key, float fallback);
bool flag(const nlohmann::json& p, const char* key, bool fallback = false);
std::string str(const nlohmann::json& p, const char* key, const char* fallback = "");
ImVec2 vec2(const nlohmann::json& p, const char* key, ImVec2 fallback = {0, 0});
ImVec4 rect(const nlohmann::json& p, const char* key);  // x, y, w, h
bool hasColour(const nlohmann::json& p, const char* key);
ImVec4 colour(const nlohmann::json& p, const char* key, ImVec4 fallback);
ImVec4 parseColour(const std::string& hex, ImVec4 fallback);
