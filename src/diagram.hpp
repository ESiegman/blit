#pragma once
// Mermaid diagrams drawn as pictures, for the Markdown preview. A diagram
// is laid out here, in points, into a drawing (shapes and text, in the
// preview's colours and the editor's font), and the drawing is drawn into
// pixels by blit itself: shapes by its rasterizer (raster.hpp), text from
// the font's own outlines (FreeType reads them; they are filled like any
// shape). No browser, no JavaScript, no SVG library: the same pixels on
// every system.
//
// Each kind of diagram lays itself out in a file of its own (flowcharts
// and state diagrams: flowchart.cpp; class and entity relationship
// diagrams, in the same layers: classdiagram.cpp), with what is here: the
// drawing, the text measurer, shapes, arrowheads and curves, and the
// statements of a Mermaid source. diagram_make() picks the kind;
// diagram_paint() draws.
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "font.hpp"
#include "markdown.hpp"
#include "raster.hpp"

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace blit {

struct DiagramColour {
    float r = 0, g = 0, b = 0, a = 0;  // a = 0: none
    bool none() const { return a <= 0; }
};

// The colours a diagram is drawn in: the preview's palette (MdInk), and the
// background it is drawn over (the picture itself is clear around them).
struct DiagramPalette {
    float ink[kMdInkCount][3] = {};
    float bg[3] = {};
    DiagramColour of(u8 which, float alpha = 1) const;
    // The background, k of the way to an ink: a tone that is the same in a
    // dark theme and a light one.
    DiagramColour tone(u8 which, float k) const;
    bool dark() const { return bg[0] * 0.3f + bg[1] * 0.59f + bg[2] * 0.11f < 0.5f; }
};

// In a line of text, marks that start and end bold and italic (from a
// Markdown string's ** and *).
constexpr u32 kDiagramBold = 2, kDiagramItalic = 3;

// The font text is measured and drawn in: a face of its own (FreeType's
// faces are not to be used by two threads, and this one is used by the
// preview's), and for characters it lacks, the editor's fallbacks (each
// opened the first time it is needed), then those fontconfig names; a
// character none has is a box. Without a face (it could not be opened)
// text is measured as a monospace font would be, and drawn as boxes.
class DiagramFont {
public:
    DiagramFont() = default;
    DiagramFont(const DiagramFont&) = delete;
    DiagramFont& operator=(const DiagramFont&) = delete;
    ~DiagramFont();
    bool open(const std::string& path, int index, const std::vector<FontFile>& fallbacks = {});
    bool same(const std::string& path, int index, const std::vector<FontFile>& fallbacks) const { return path == path_ && index == index_ && fallbacks == fallback_files_; }
    bool ok() const { return face_ != nullptr; }
    // In points at `size` points (an em): how wide text is, and how far
    // above and below its baseline a line reaches.
    float width(std::string_view text, float size, bool bold = false) const;
    float ascent(float size) const { return ascent_ * size; }
    float descent(float size) const { return descent_ * size; }
    float line(float size) const { return (ascent_ + descent_) * size * 1.18f; }
    // The outlines of text whose baseline starts at (x, y).
    void outline(std::string_view text, float x, float y, float size, bool bold, RasterPath* out) const;

private:
    struct Glyph {
        float advance = 0;  // in ems
        RasterPath shape;   // in ems, y down
        bool done = false;
    };
    struct Fallback {
        FontFile file;
        FT_Face face = nullptr;
        float units = 1000;
        bool tried = false;
    };
    const Glyph& glyph(u32 cp) const;
    FT_Face face_for(u32 cp, unsigned* gi, float* units) const;
    FT_Library lib_ = nullptr;
    FT_Face face_ = nullptr;
    std::string path_;
    int index_ = -1;
    float ascent_ = 0.8f, descent_ = 0.2f, units_ = 1000;
    std::vector<FontFile> fallback_files_;
    mutable std::vector<Fallback> fallbacks_;
    mutable std::map<u32, Glyph> glyphs_;
};

// What is drawn: shapes (filled, outlined or both) and lines of text, in
// order, in points.
struct DiagramItem {
    enum Kind : u8 { kShape, kText };
    u8 kind = kShape;
    RasterPath path;
    DiagramColour fill, stroke;
    float width = 0;          // the outline's (points)
    std::vector<float> dash;  // on, off, ... (points)
    u8 cap = RasterStroke::kButt, join = RasterStroke::kRoundJoin;
    // Text: its line, its baseline's left end, its size.
    std::string text;
    float x = 0, y = 0, size = 0;
    bool bold = false;
    bool italic = false;  // (slanted: the font's own letters, leaning)
};

struct Drawing {
    float w = 0, h = 0;  // its size (points), from 0, 0
    std::vector<DiagramItem> items;
    void shape(RasterPath p, DiagramColour fill, DiagramColour stroke = {}, float width = 0, std::vector<float> dash = {}, u8 cap = RasterStroke::kButt);
    // A line of text, `align` 0: from x, 1: centred on it, 2: ending at it.
    void text(std::string_view s, float x, float baseline, float size, DiagramColour c, const DiagramFont& font, int align = 0, bool bold = false);
};

// Where things were put, for the tests: boxes by name, and each edge's
// ends and the points along it.
struct DiagramGeometry {
    struct Box {
        std::string kind, name;  // "node", "cluster", "label"
        float x = 0, y = 0, w = 0, h = 0;
    };
    struct Line {
        std::string from, to;
        std::vector<RasterPoint> pts;  // along the line as drawn, its first and last at its ends
    };
    std::vector<Box> boxes;
    std::vector<Line> lines;
    int edges = -1;  // how many edges the source has that are drawn (each should have a line)
    std::string dump() const;
};

// What a diagram is laid out for: the font and its size, the colours, and
// the room it has across (points).
struct DiagramStyle {
    const DiagramFont* font = nullptr;
    float size = 13;      // the text's size, points
    float room = 600;     // how wide it may be (it is made smaller to fit, if it is wider)
    DiagramPalette palette;
    std::stop_token stop;  // (asked to stop: it gives up, "stopped")
    float unit() const { return size / 16; }  // (Mermaid's sizes are for 16 pixel text: in proportion)
};

// A source laid out as a drawing. kDiagramNone: not a kind drawn as a
// picture (why in `err`: not drawn yet); kDiagramError: a mistake in it
// (why in `err`).
enum DiagramMade : u8 { kDiagramNone, kDiagramDrawn, kDiagramError };
u8 diagram_make(std::string_view source, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo = nullptr);

// The drawing in pixels: `scale` pixels to a point, RGBA (colours
// multiplied by alpha), clear where nothing is drawn.
void diagram_paint(const Drawing& d, float scale, const DiagramFont& font, RasterImage* out);

// ---------------------------------------------------------------- for each kind

struct DiagramVec {
    float x = 0, y = 0;
};

// How deep subgraphs (composite states, namespaces) may be nested: each is
// laid out by recursion, on the preview's worker, whose stack is small
// (the system's 512 KB on a Mac). Deeper than any diagram a person draws,
// and a mistake, said with its line.
constexpr int kDiagramDeepest = 32;

// Mermaid's statements: its lines (and what ; separates, outside quotes),
// without comments, front matter (its title in `title`) or directives.
std::vector<std::string> diagram_statements(std::string_view src, bool split_semicolons, std::string* title = nullptr);
std::string_view diagram_trim(std::string_view s);
std::string diagram_lower(std::string_view s);
// A label as it is shown: its quotes and Markdown's backticks off, <br>
// and \n new lines, a few entities and #codes; its lines.
std::vector<std::string> diagram_label(std::string_view s);
// Lines of text no wider than `most` points (broken at spaces, a word too
// long broken where it must be).
std::vector<std::string> diagram_wrap(const std::vector<std::string>& lines, float most, float size, const DiagramFont& font);
// A CSS colour (#rgb, #rrggbb, #rrggbbaa, rgb(), rgba(), a few names).
bool diagram_colour(std::string_view s, DiagramColour* out);

// Shapes of nodes, in a box (x, y its top left), as outlines.
enum DiagramShape : u8 {
    kShapeRect,
    kShapeRound,
    kShapeStadium,
    kShapeSubroutine,
    kShapeCylinder,
    kShapeCircle,
    kShapeDoubleCircle,
    kShapeDiamond,
    kShapeHexagon,
    kShapeLeanRight,
    kShapeLeanLeft,
    kShapeTrapezoid,     // wide at the bottom
    kShapeTrapezoidAlt,  // wide at the top
    kShapeAsymmetric,
    kShapeState,  // a state's box (rounder)
    kShapeStart,  // [*], where a state machine begins (a dot)
    kShapeEnd,    // and ends (a dot in a ring)
    kShapeBar,    // a fork or a join
    kShapeNote,
    kShapeCount
};
void diagram_shape(u8 shape, float x, float y, float w, float h, float unit, RasterPath* out);
// The lines drawn over a shape besides its outline (a subroutine's sides, a
// cylinder's rim), or nothing.
bool diagram_shape_extra(u8 shape, float x, float y, float w, float h, float unit, RasterPath* out);
// The size a shape takes around text tw by th, padded by px and py.
DiagramVec diagram_shape_size(u8 shape, float tw, float th, float px, float py);
// Where a line from `from` (outside the shape) towards `to` (inside it)
// first meets its outline; `to` if it never does.
DiagramVec diagram_shape_hit(u8 shape, float x, float y, float w, float h, float unit, DiagramVec from, DiagramVec to);

// Ends of lines.
enum DiagramHead : u8 { kHeadNone, kHeadArrow, kHeadCircle, kHeadCross, kHeadOpen };
// A head at `tip` pointing along (dx, dy) (a unit vector): drawn, and how
// far back from the tip the line should stop.
float diagram_head(Drawing& d, u8 head, DiagramVec tip, DiagramVec dir, float size, DiagramColour c, float width);
float diagram_head_length(u8 head, float size);
// A smooth curve through points, leaving the first and reaching the last
// along their axes (unit vectors; 0, 0: along the line), the points between
// kept from doubling back along `axis` (0, 0: the first's).
void diagram_curve(const std::vector<DiagramVec>& pts, DiagramVec axis_start, DiagramVec axis_end, RasterPath* out, DiagramVec axis = {});
// A curve's points (for the tests' geometry): `n` of them along it.
std::vector<RasterPoint> diagram_sample(const RasterPath& p, int n);

// Flowcharts (graph, flowchart) and state diagrams (flowchart.cpp).
bool flowchart_layout(const std::vector<std::string>& st, bool state, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo);
// Sequence diagrams (sequence.cpp).
bool sequence_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo);
// Pie charts (pie.cpp).
bool pie_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo);

// A graph laid out in layers as flowcharts are (flowchart.cpp), for the
// kinds that draw their own nodes and lines: each node's size in, its
// place out; each edge's points out, its ends on its nodes' borders.
// False if it gave up (why in `err`: too large to draw, or asked to stop).
struct LayeredGraph {
    struct Node {
        std::string id;
        float w = 0, h = 0;
        u8 shape = kShapeRect;  // (where its edges meet it)
        int cluster = -1;       // the innermost cluster it is in
        int note_of = -1;       // a note: the node it is beside
        bool note_left = false;
        float x = 0, y = 0;  // laid out: its middle
    };
    struct Edge {
        int from = 0, to = 0;
        std::vector<std::string> label;  // its lines (wrapped, once laid out)
        int minlen = 1;
        // A loop (from a node to itself): how far its ends run straight out
        // of the node, how wide they are across (half), and how much more
        // the words beside them take. (Several loops on a node go each round
        // the one before.)
        float end_reach = 0, end_half = 0, end_word = 0;
        // Laid out: its points from `from` to `to` (a loop: where it leaves,
        // where its straight part ends, two control points, where its
        // straight part back begins, where it comes back), the way the
        // ranks go along it at both ends, and its label's middle and size.
        std::vector<DiagramVec> pts;
        DiagramVec axis;
        bool loop = false;
        DiagramVec out;  // a loop: the way out of its node
        DiagramVec label_at;
        float label_w = 0, label_h = 0;
    };
    struct Cluster {
        std::string id;
        std::vector<std::string> title;  // drawn at its top, a line (and a little) each
        int parent = -1;
        float x = 0, y = 0, w = 0, h = 0;  // laid out
        float title_x = 0;  // laid out: the middle of its title (edges that come in at its top pass beside it there)
    };
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<Cluster> clusters;
    char dir = 'D';  // D (top to bottom), U, R (left to right), L
    float sep = 1;   // the gaps between nodes and between ranks, times this
    float w = 0, h = 0;  // laid out: the whole, from 0, 0
};
bool diagram_layered(LayeredGraph* g, const DiagramStyle& style, std::string* err);

// Class diagrams and entity relationship diagrams (classdiagram.cpp).
bool classdiagram_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo);
bool erdiagram_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo);

}  // namespace blit
