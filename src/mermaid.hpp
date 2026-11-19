#pragma once
// Mermaid diagrams drawn in box-drawing characters for the Markdown
// preview: sequence diagrams, and pie charts (as bars). Laid out here, in
// cells, with no browser and no JavaScript. (Flowcharts and state diagrams
// are drawn as pictures: diagram.hpp.)
#include <string>
#include <string_view>
#include <vector>

#include "markdown.hpp"

namespace blit {

// The diagram as rows, as narrow as `width` if it can be. False, and why
// in `note`, when it cannot be drawn (a kind not drawn yet, or a mistake in
// it; "" for a kind drawn as a picture); `note` may also say something
// about one that is drawn (cut at the right).
bool mermaid_draw(std::string_view source, u32 width, std::vector<MdRow>* rows, std::string* note);

}  // namespace blit
