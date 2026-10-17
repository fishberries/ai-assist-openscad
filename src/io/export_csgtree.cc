/*
 * Exports the evaluated CSG term tree: every leaf carries the mesh OpenSCAD's own
 * GeometryEvaluator produced for it, plus its transform and colour, and the boolean
 * operations are left unevaluated.
 *
 * This is the data OpenSCAD's own preview renderer consumes. OpenCSGRenderer takes
 * CSGProducts built from exactly these CSGLeaf objects (polyset + matrix + colour) and
 * composes the booleans on the GPU; it never generates primitive geometry itself. A
 * consumer of this file can therefore reproduce OpenSCAD's F5 preview without
 * reimplementing any part of the SCAD language.
 *
 * Deliberately exports the tree BEFORE CSGTreeNormalizer runs. Normalization is capped
 * by RenderSettings::openCSGTermLimit and can bail out, which would silently drop
 * geometry - the precise failure mode this export exists to eliminate. Consumers that
 * want products should normalize themselves, where the term limit is their own choice.
 *
 * Format: text, line oriented, versioned by the first line. Text was chosen over a
 * binary packing so the output can be inspected directly while the consumer is being
 * written; it is an implementation detail behind the version number and may become
 * binary without changing the data model.
 *
 *   GINSU-CSG-TREE 1
 *   root <nodeIndex>              -- -1 when the tree is empty
 *   leaves <count>
 *   nodes <count>
 *   L <leafIndex> <label>         -- begins a leaf record
 *   LC <valid> <r> <g> <b> <a>    -- valid is 0 or 1; components are 0..1 floats
 *   LM <m0> .. <m15>              -- 4x4, ROW MAJOR
 *   LF <faceCount> <wasTriangular>
 *   LT <triangleCount>
 *   T <x0> <y0> <z0> <x1> ... <z2>   -- repeated triangleCount times
 *   N <nodeIndex> leaf <leafIndex>
 *   N <nodeIndex> union|difference|intersection <leftNode> <rightNode>
 *
 * Nodes are emitted in post-order, so a child's index always precedes its parent's.
 *
 * IMPORTANT: triangle vertices are in the leaf's LOCAL space. The consumer must apply
 * LM. This matches CSGLeaf semantics, where the renderer multiplies by the leaf matrix.
 * Baking the transform here instead would be a silent double-transform hazard for any
 * consumer that also honours the matrix.
 */

#include "io/export.h"

#include <cstddef>
#include <iomanip>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/CSGNode.h"
#include "core/enums.h"
#include "geometry/PolySet.h"
#include "geometry/linalg.h"

namespace {

// Labels come from STR(node.name(), node.index()) and are well behaved, but a label is
// read back as a single token so any whitespace would desynchronize the parser.
std::string sanitizeLabel(const std::string& label)
{
  if (label.empty()) return "-";
  std::string out = label;
  for (char& c : out) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') c = '_';
  }
  return out;
}

const char *operatorName(OpenSCADOperator type)
{
  switch (type) {
  case OpenSCADOperator::UNION: return "union";
  case OpenSCADOperator::DIFFERENCE: return "difference";
  case OpenSCADOperator::INTERSECTION: return "intersection";
  // The evaluator turns minkowski/hull/fill/resize into leaves via the geometry
  // evaluator rather than operation nodes, so these should be unreachable here. Name
  // them anyway rather than emitting a wrong operator: a consumer that meets one should
  // be able to fail loudly instead of silently treating it as a union.
  case OpenSCADOperator::MINKOWSKI: return "minkowski";
  case OpenSCADOperator::HULL: return "hull";
  case OpenSCADOperator::FILL: return "fill";
  case OpenSCADOperator::RESIZE: return "resize";
  }
  return "unknown";
}

class CsgTreeExporter
{
public:
  explicit CsgTreeExporter(std::ostream& output) : output_(output) {}

  // Returns the emitted node index, or -1 when nothing could be emitted.
  int walk(const std::shared_ptr<CSGNode>& node)
  {
    if (!node) return -1;

    if (const auto leaf = std::dynamic_pointer_cast<CSGLeaf>(node)) {
      return emitLeafNode(*leaf);
    }

    if (const auto op = std::dynamic_pointer_cast<CSGOperation>(node)) {
      const int left = walk(op->left());
      const int right = walk(op->right());
      if (left < 0 && right < 0) return -1;
      // A half-missing boolean is reported rather than quietly collapsed to its
      // surviving side, which is how the consumer this replaces lost a shape.
      if (left < 0 || right < 0) {
        const int survivor = left < 0 ? right : left;
        bodyLines_.push_back(std::string("# WARNING dropped-operand ") + operatorName(op->getType()));
        return survivor;
      }
      return emitOperationNode(op->getType(), left, right);
    }

    bodyLines_.push_back("# WARNING unknown-node-type");
    return -1;
  }

  void flush(int rootIndex)
  {
    output_ << "GINSU-CSG-TREE 1\n";
    output_ << "root " << rootIndex << "\n";
    output_ << "leaves " << leafCount_ << "\n";
    output_ << "nodes " << nodeCount_ << "\n";
    for (const auto& line : bodyLines_) {
      output_ << line << "\n";
    }
  }

private:
  int emitLeafNode(const CSGLeaf& leaf)
  {
    const int leafIndex = leafCount_++;

    {
      std::ostringstream line;
      line << "L " << leafIndex << " " << sanitizeLabel(leaf.label);
      bodyLines_.push_back(line.str());
    }

    {
      std::ostringstream line;
      line << std::setprecision(9);
      const bool valid = leaf.color.isValid();
      line << "LC " << (valid ? 1 : 0) << " " << leaf.color.r() << " " << leaf.color.g() << " "
           << leaf.color.b() << " " << leaf.color.a();
      bodyLines_.push_back(line.str());
    }

    {
      // Full precision: there are only sixteen of these per leaf, and a transform that
      // loses digits moves geometry.
      std::ostringstream line;
      line << std::setprecision(17);
      line << "LM";
      for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
          line << " " << leaf.matrix(row, col);
        }
      }
      bodyLines_.push_back(line.str());
    }

    emitTriangles(leaf.polyset);

    return emitNodeLine(std::string("leaf ") + std::to_string(leafIndex));
  }

  void emitTriangles(const std::shared_ptr<const PolySet>& polyset)
  {
    if (!polyset) {
      bodyLines_.push_back("LF 0 0");
      bodyLines_.push_back("LT 0");
      return;
    }

    {
      std::ostringstream line;
      line << "LF " << polyset->indices.size() << " " << (polyset->isTriangular() ? 1 : 0);
      bodyLines_.push_back(line.str());
    }

    // Faces may be n-gons; fan triangulate. PolySet faces are planar and convex enough
    // for a fan in the preview pipeline, which is what the GL renderers assume too.
    std::vector<std::string> triangles;
    for (const auto& face : polyset->indices) {
      if (face.size() < 3) continue;
      for (std::size_t i = 1; i + 1 < face.size(); ++i) {
        const int ia = face[0];
        const int ib = face[i];
        const int ic = face[i + 1];
        if (ia < 0 || ib < 0 || ic < 0) continue;
        if (static_cast<std::size_t>(ia) >= polyset->vertices.size() ||
            static_cast<std::size_t>(ib) >= polyset->vertices.size() ||
            static_cast<std::size_t>(ic) >= polyset->vertices.size()) {
          continue;
        }
        const Vector3d& a = polyset->vertices[ia];
        const Vector3d& b = polyset->vertices[ib];
        const Vector3d& c = polyset->vertices[ic];
        std::ostringstream line;
        line << std::setprecision(9);
        line << "T " << a.x() << " " << a.y() << " " << a.z() << " " << b.x() << " " << b.y() << " "
             << b.z() << " " << c.x() << " " << c.y() << " " << c.z();
        triangles.push_back(line.str());
      }
    }

    bodyLines_.push_back("LT " + std::to_string(triangles.size()));
    for (auto& triangle : triangles) {
      bodyLines_.push_back(std::move(triangle));
    }
  }

  int emitOperationNode(OpenSCADOperator type, int left, int right)
  {
    std::ostringstream tail;
    tail << operatorName(type) << " " << left << " " << right;
    return emitNodeLine(tail.str());
  }

  int emitNodeLine(const std::string& tail)
  {
    const int nodeIndex = nodeCount_++;
    bodyLines_.push_back("N " + std::to_string(nodeIndex) + " " + tail);
    return nodeIndex;
  }

  std::ostream& output_;
  std::vector<std::string> bodyLines_;
  int leafCount_ = 0;
  int nodeCount_ = 0;
};

}  // namespace

void export_csgtree(const std::shared_ptr<CSGNode>& root, std::ostream& output)
{
  CsgTreeExporter exporter(output);
  const int rootIndex = root ? exporter.walk(root) : -1;
  exporter.flush(rootIndex);
}
