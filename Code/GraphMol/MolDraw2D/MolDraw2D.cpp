//
//  Copyright (C) 2014-2020 David Cosgrove and Greg Landrum
//
//   @@ All Rights Reserved @@
//  This file is part of the RDKit.
//  The contents are covered by the terms of the BSD license
//  which is included in the file license.txt, found at the root
//  of the RDKit source tree.
//
// Original author: David Cosgrove (AstraZeneca)
// 27th May 2014
//
// Extensively modified by Greg Landrum
//

#include <GraphMol/QueryOps.h>
#include <GraphMol/MolDraw2D/DrawText.h>
#include <GraphMol/MolDraw2D/MolDraw2D.h>
#include <GraphMol/MolDraw2D/MolDraw2DDetails.h>
#include <GraphMol/MolDraw2D/MolDraw2DUtils.h>
#include <GraphMol/ChemReactions/ReactionParser.h>
#include <GraphMol/FileParsers/MolSGroupParsing.h>
#include <GraphMol/Depictor/RDDepictor.h>
#include <Geometry/point.h>
#include <Geometry/Transform2D.h>
#include <Numerics/SquareMatrix.h>
#include <Numerics/Matrix.h>

#include <GraphMol/MolTransforms/MolTransforms.h>
#include <GraphMol/FileParsers/FileParserUtils.h>
#include <GraphMol/MolEnumerator/LinkNode.h>

#include <Geometry/Transform3D.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <memory>

#include <boost/lexical_cast.hpp>
#include <boost/tuple/tuple_comparison.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/format.hpp>

using namespace boost;
using namespace std;

namespace RDKit {

namespace {
// ****************************************************************************
// calculate normalised perpendicular to vector between two coords
Point2D calcPerpendicular(const Point2D &cds1, const Point2D &cds2) {
  double bv[2] = {cds1.x - cds2.x, cds1.y - cds2.y};
  double perp[2] = {-bv[1], bv[0]};
  double perp_len = sqrt(perp[0] * perp[0] + perp[1] * perp[1]);
  perp[0] /= perp_len;
  perp[1] /= perp_len;

  return Point2D(perp[0], perp[1]);
}

// ****************************************************************************
// calculate normalised perpendicular to vector between two coords, such that
// it's inside the angle made between (1 and 2) and (2 and 3).
Point2D calcInnerPerpendicular(const Point2D &cds1, const Point2D &cds2,
                               const Point2D &cds3) {
  Point2D perp = calcPerpendicular(cds1, cds2);
  double v1[2] = {cds1.x - cds2.x, cds1.y - cds2.y};
  double v2[2] = {cds2.x - cds3.x, cds2.y - cds3.y};
  double obv[2] = {v1[0] - v2[0], v1[1] - v2[1]};

  // if dot product of centre_dir and perp < 0.0, they're pointing in opposite
  // directions, so reverse perp
  if (obv[0] * perp.x + obv[1] * perp.y < 0.0) {
    perp.x *= -1.0;
    perp.y *= -1.0;
  }

  return perp;
}

// ****************************************************************************
// cds1 and cds2 are 2 atoms in a ring.  Returns the perpendicular pointing
// into the ring
Point2D bondInsideRing(const ROMol &mol, const Bond &bond, const Point2D &cds1,
                       const Point2D &cds2,
                       const std::vector<Point2D> &at_cds) {
  vector<size_t> bond_in_rings;
  auto bond_rings = mol.getRingInfo()->bondRings();
  for (size_t i = 0; i < bond_rings.size(); ++i) {
    if (find(bond_rings[i].begin(), bond_rings[i].end(), bond.getIdx()) !=
        bond_rings[i].end()) {
      bond_in_rings.push_back(i);
    }
  }

  // find another bond in the ring connected to bond, use the
  // other end of it as the 3rd atom.
  auto calc_perp = [&](const Bond *bond, const INT_VECT &ring) -> Point2D * {
    Atom *bgn_atom = bond->getBeginAtom();
    for (const auto &nbri2 : make_iterator_range(mol.getAtomBonds(bgn_atom))) {
      const Bond *bond2 = mol[nbri2];
      if (bond2 == bond) {
        continue;
      }
      if (find(ring.begin(), ring.end(), bond2->getIdx()) != ring.end()) {
        int atom3 = bond2->getOtherAtomIdx(bond->getBeginAtomIdx());
        Point2D *ret = new Point2D;
        *ret = calcInnerPerpendicular(cds1, cds2, at_cds[atom3]);
        return ret;
      }
    }
    return nullptr;
  };

  if (bond_in_rings.size() > 1) {
    // bond is in more than 1 ring.  Choose one that is the same aromaticity
    // as the bond, so that if bond is aromatic, the double bond is inside
    // the aromatic ring.  This is important for morphine, for example,
    // where there are fused aromatic and aliphatic rings.
    // morphine: CN1CC[C@]23c4c5ccc(O)c4O[C@H]2[C@@H](O)C=C[C@H]3[C@H]1C5
    for (size_t i = 0; i < bond_in_rings.size(); ++i) {
      auto ring = bond_rings[bond_in_rings[i]];
      bool ring_ok = true;
      for (auto bond_idx : ring) {
        const Bond *bond2 = mol.getBondWithIdx(bond_idx);
        if (bond.getIsAromatic() != bond2->getIsAromatic()) {
          ring_ok = false;
          break;
        }
      }
      if (!ring_ok) {
        continue;
      }
      Point2D *ret = calc_perp(&bond, ring);
      if (ret) {
        Point2D real_ret(*ret);
        delete ret;
        return real_ret;
      }
    }
  }

  // either bond is in 1 ring, or we couldn't decide above, so just use the
  // first one
  auto ring = bond_rings[bond_in_rings.front()];
  Point2D *ret = calc_perp(&bond, ring);
  if (ret) {
    Point2D real_ret(*ret);
    delete ret;
    return real_ret;
  }

  // failsafe that it will hopefully never see.
  return calcPerpendicular(cds1, cds2);
}

// ****************************************************************************
bool isLinearAtom(const Atom &atom, const std::vector<Point2D> &at_cds) {
  if (atom.getDegree() == 2) {
    Point2D bond_vecs[2];
    Bond::BondType bts[2];
    Point2D const &at1_cds = at_cds[atom.getIdx()];
    ROMol const &mol = atom.getOwningMol();
    int i = 0;
    for (const auto &nbr : make_iterator_range(mol.getAtomNeighbors(&atom))) {
      Point2D bond_vec = at1_cds.directionVector(at_cds[nbr]);
      bond_vec.normalize();
      bond_vecs[i] = bond_vec;
      bts[i] = mol.getBondBetweenAtoms(atom.getIdx(), nbr)->getBondType();
      ++i;
    }
    return (bts[0] == bts[1] && bond_vecs[0].dotProduct(bond_vecs[1]) < -0.95);
  }
  return false;
}

// ****************************************************************************
// cds1 and cds2 are 2 atoms in a chain double bond.  Returns the
// perpendicular pointing into the inside of the bond
Point2D bondInsideDoubleBond(const ROMol &mol, const Bond &bond,
                             const std::vector<Point2D> &at_cds) {
  // a chain double bond, where it looks nicer IMO if the 2nd line is inside
  // the angle of outgoing bond. Unless it's an allene, where nothing
  // looks great.
  const Atom *at1 = bond.getBeginAtom();
  const Atom *at2 = bond.getEndAtom();
  const Atom *bond_atom, *end_atom;
  if (at1->getDegree() > 1) {
    bond_atom = at1;
    end_atom = at2;
  } else {
    bond_atom = at2;
    end_atom = at1;
  }
  int at3 = -1;  // to stop the compiler whinging.
  for (const auto &nbri2 : make_iterator_range(mol.getAtomBonds(bond_atom))) {
    const Bond *bond2 = mol[nbri2];
    if (&bond != bond2) {
      at3 = bond2->getOtherAtomIdx(bond_atom->getIdx());
      break;
    }
  }

  return calcInnerPerpendicular(at_cds[end_atom->getIdx()],
                                at_cds[bond_atom->getIdx()], at_cds[at3]);
}

// ****************************************************************************
void calcDoubleBondLines(const ROMol &mol, double offset, const Bond &bond,
                         const Point2D &at1_cds, const Point2D &at2_cds,
                         const std::vector<Point2D> &at_cds, Point2D &l1s,
                         Point2D &l1f, Point2D &l2s, Point2D &l2f) {
  // the percent shorter that the extra bonds in a double bond are
  const double multipleBondTruncation = 0.15;
  Atom *at1 = bond.getBeginAtom();
  Atom *at2 = bond.getEndAtom();
  Point2D perp;
  if (1 == at1->getDegree() || 1 == at2->getDegree() ||
      isLinearAtom(*at1, at_cds) || isLinearAtom(*at2, at_cds)) {
    perp = calcPerpendicular(at1_cds, at2_cds) * offset;
    l1s = at1_cds + perp;
    l1f = at2_cds + perp;
    l2s = at1_cds - perp;
    l2f = at2_cds - perp;
  } else if ((Bond::EITHERDOUBLE == bond.getBondDir()) ||
             (Bond::STEREOANY == bond.getStereo())) {
    // crossed bond
    perp = calcPerpendicular(at1_cds, at2_cds) * offset;
    l1s = at1_cds + perp;
    l1f = at2_cds - perp;
    l2s = at1_cds - perp;
    l2f = at2_cds + perp;
  } else {
    l1s = at1_cds;
    l1f = at2_cds;
    offset *= 2.0;
    if (mol.getRingInfo()->numBondRings(bond.getIdx())) {
      // in a ring, we need to draw the bond inside the ring
      perp = bondInsideRing(mol, bond, at1_cds, at2_cds, at_cds);
    } else {
      perp = bondInsideDoubleBond(mol, bond, at_cds);
    }
    Point2D bv = at1_cds - at2_cds;
    l2s = at1_cds - bv * multipleBondTruncation + perp * offset;
    l2f = at2_cds + bv * multipleBondTruncation + perp * offset;
  }
}

// ****************************************************************************
void calcTripleBondLines(double offset, const Bond &bond,
                         const Point2D &at1_cds, const Point2D &at2_cds,
                         Point2D &l1s, Point2D &l1f, Point2D &l2s,
                         Point2D &l2f) {
  // the percent shorter that the extra bonds in a double bond are
  const double multipleBondTruncation = 0.15;

  Atom *at1 = bond.getBeginAtom();
  Atom *at2 = bond.getEndAtom();

  // 2 lines, a bit shorter and offset on the perpendicular
  double dbo = 2.0 * offset;
  Point2D perp = calcPerpendicular(at1_cds, at2_cds);
  double end1_trunc = 1 == at1->getDegree() ? 0.0 : multipleBondTruncation;
  double end2_trunc = 1 == at2->getDegree() ? 0.0 : multipleBondTruncation;
  Point2D bv = at1_cds - at2_cds;
  l1s = at1_cds - (bv * end1_trunc) + perp * dbo;
  l1f = at2_cds + (bv * end2_trunc) + perp * dbo;
  l2s = at1_cds - (bv * end1_trunc) - perp * dbo;
  l2f = at2_cds + (bv * end2_trunc) - perp * dbo;
}

void getBondHighlightsForAtoms(const ROMol &mol,
                               const vector<int> &highlight_atoms,
                               vector<int> &highlight_bonds) {
  highlight_bonds.clear();
  for (auto ai = highlight_atoms.begin(); ai != highlight_atoms.end(); ++ai) {
    for (auto aj = ai + 1; aj != highlight_atoms.end(); ++aj) {
      const Bond *bnd = mol.getBondBetweenAtoms(*ai, *aj);
      if (bnd) {
        highlight_bonds.push_back(bnd->getIdx());
      }
    }
  }
}
void centerMolForDrawing(RWMol &mol, int confId) {
  auto &conf = mol.getConformer(confId);
  RDGeom::Transform3D tf;
  auto centroid = MolTransforms::computeCentroid(conf);
  centroid *= -1;
  tf.SetTranslation(centroid);
  MolTransforms::transformConformer(conf, tf);
  MolTransforms::transformMolSubstanceGroups(mol, tf);
}
}  // namespace

// ****************************************************************************
MolDraw2D::MolDraw2D(int width, int height, int panelWidth, int panelHeight)
    : needs_scale_(true),
      width_(width),
      height_(height),
      panel_width_(panelWidth > 0 ? panelWidth : width),
      panel_height_(panelHeight > 0 ? panelHeight : height),
      legend_height_(0),
      scale_(1.0),
      x_min_(0.0),
      y_min_(0.0),
      x_range_(0.0),
      y_range_(0.0),
      x_trans_(0.0),
      y_trans_(0.0),
      x_offset_(0),
      y_offset_(0),
      fill_polys_(true),
      activeMolIdx_(-1),
      activeAtmIdx1_(-1),
      activeAtmIdx2_(-1) {}

// ****************************************************************************
MolDraw2D::~MolDraw2D() {}

// ****************************************************************************
void MolDraw2D::drawMolecule(const ROMol &mol,
                             const vector<int> *highlight_atoms,
                             const map<int, DrawColour> *highlight_atom_map,
                             const std::map<int, double> *highlight_radii,
                             int confId) {
  drawMolecule(mol, "", highlight_atoms, highlight_atom_map, highlight_radii,
               confId);
}

// ****************************************************************************
void MolDraw2D::drawMolecule(const ROMol &mol, const std::string &legend,
                             const vector<int> *highlight_atoms,
                             const map<int, DrawColour> *highlight_atom_map,
                             const std::map<int, double> *highlight_radii,
                             int confId) {
  vector<int> highlight_bonds;
  if (highlight_atoms) {
    getBondHighlightsForAtoms(mol, *highlight_atoms, highlight_bonds);
  }
  drawMolecule(mol, legend, highlight_atoms, &highlight_bonds,
               highlight_atom_map, nullptr, highlight_radii, confId);
}

// ****************************************************************************
void MolDraw2D::doContinuousHighlighting(
    const ROMol &mol, const vector<int> *highlight_atoms,
    const vector<int> *highlight_bonds,
    const map<int, DrawColour> *highlight_atom_map,
    const map<int, DrawColour> *highlight_bond_map,
    const std::map<int, double> *highlight_radii) {
  PRECONDITION(activeMolIdx_ >= 0, "bad active mol");

  int orig_lw = lineWidth();
  int tgt_lw = getHighlightBondWidth(-1, nullptr);
  if (tgt_lw < 2) {
    tgt_lw = 2;
  }

  bool orig_fp = fillPolys();
  if (highlight_bonds) {
    for (auto this_at : mol.atoms()) {
      int this_idx = this_at->getIdx();
      for (const auto &nbri : make_iterator_range(mol.getAtomBonds(this_at))) {
        const Bond *bond = mol[nbri];
        int nbr_idx = bond->getOtherAtomIdx(this_idx);
        if (nbr_idx < static_cast<int>(at_cds_[activeMolIdx_].size()) &&
            nbr_idx > this_idx) {
          if (std::find(highlight_bonds->begin(), highlight_bonds->end(),
                        bond->getIdx()) != highlight_bonds->end()) {
            DrawColour col = drawOptions().highlightColour;
            if (highlight_bond_map &&
                highlight_bond_map->find(bond->getIdx()) !=
                    highlight_bond_map->end()) {
              col = highlight_bond_map->find(bond->getIdx())->second;
            }
            setLineWidth(tgt_lw);
            Point2D at1_cds = at_cds_[activeMolIdx_][this_idx];
            Point2D at2_cds = at_cds_[activeMolIdx_][nbr_idx];
            bool orig_slw = drawOptions().scaleBondWidth;
            drawOptions().scaleBondWidth =
                drawOptions().scaleHighlightBondWidth;
            drawLine(at1_cds, at2_cds, col, col);
            drawOptions().scaleBondWidth = orig_slw;
          }
        }
      }
    }
  }
  if (highlight_atoms) {
    if (!drawOptions().fillHighlights) {
      // we need a narrower circle
      setLineWidth(tgt_lw / 2);
    }
    for (auto this_at : mol.atoms()) {
      int this_idx = this_at->getIdx();
      if (std::find(highlight_atoms->begin(), highlight_atoms->end(),
                    this_idx) != highlight_atoms->end()) {
        DrawColour col = drawOptions().highlightColour;
        if (highlight_atom_map &&
            highlight_atom_map->find(this_idx) != highlight_atom_map->end()) {
          col = highlight_atom_map->find(this_idx)->second;
        }
        vector<DrawColour> cols(1, col);
        drawHighlightedAtom(this_idx, cols, highlight_radii);
      }
    }
  }
  setLineWidth(orig_lw);
  setFillPolys(orig_fp);
}

// ****************************************************************************
void MolDraw2D::drawMolecule(const ROMol &mol,
                             const vector<int> *highlight_atoms,
                             const vector<int> *highlight_bonds,
                             const map<int, DrawColour> *highlight_atom_map,
                             const map<int, DrawColour> *highlight_bond_map,
                             const std::map<int, double> *highlight_radii,
                             int confId) {
  int origWidth = lineWidth();
  pushDrawDetails();
  setupTextDrawer();

  unique_ptr<RWMol> rwmol =
      setupMoleculeDraw(mol, highlight_atoms, highlight_radii, confId);
  ROMol const &draw_mol = rwmol ? *(rwmol) : mol;
  if (!draw_mol.getNumConformers()) {
    // clearly, the molecule is in a sorry state.
    return;
  }

  if (!pre_shapes_[activeMolIdx_].empty()) {
    MolDraw2D_detail::drawShapes(*this, pre_shapes_[activeMolIdx_]);
  }

  if (drawOptions().continuousHighlight) {
    // if we're doing continuous highlighting, start by drawing the highlights
    doContinuousHighlighting(draw_mol, highlight_atoms, highlight_bonds,
                             highlight_atom_map, highlight_bond_map,
                             highlight_radii);
    // at this point we shouldn't be doing any more highlighting, so blow out
    // those variables.  This alters the behaviour of drawBonds below.
    highlight_bonds = nullptr;
    highlight_atoms = nullptr;
  } else if (drawOptions().circleAtoms && highlight_atoms) {
    setFillPolys(drawOptions().fillHighlights);
    for (auto this_at : draw_mol.atoms()) {
      int this_idx = this_at->getIdx();
      if (std::find(highlight_atoms->begin(), highlight_atoms->end(),
                    this_idx) != highlight_atoms->end()) {
        if (highlight_atom_map &&
            highlight_atom_map->find(this_idx) != highlight_atom_map->end()) {
          setColour(highlight_atom_map->find(this_idx)->second);
        } else {
          setColour(drawOptions().highlightColour);
        }
        Point2D p1 = at_cds_[activeMolIdx_][this_idx];
        Point2D p2 = at_cds_[activeMolIdx_][this_idx];
        double radius = drawOptions().highlightRadius;
        if (highlight_radii &&
            highlight_radii->find(this_idx) != highlight_radii->end()) {
          radius = highlight_radii->find(this_idx)->second;
        }
        Point2D offset(radius, radius);
        p1 -= offset;
        p2 += offset;
        drawEllipse(p1, p2);
      }
    }
    setFillPolys(true);
  }

  drawBonds(draw_mol, highlight_atoms, highlight_atom_map, highlight_bonds,
            highlight_bond_map);

  vector<DrawColour> atom_colours;
  for (auto this_at : draw_mol.atoms()) {
    atom_colours.emplace_back(
        getColour(this_at->getIdx(), highlight_atoms, highlight_atom_map));
  }

  finishMoleculeDraw(draw_mol, atom_colours);
  // popDrawDetails();
  setLineWidth(origWidth);

  if (drawOptions().includeMetadata) {
    this->updateMetadata(draw_mol, confId);
  }
  // {
  //   Point2D p1(x_min_, y_min_), p2(x_min_ + x_range_, y_min_ + y_range_);
  //   setColour(DrawColour(0, 0, 0));
  //   setFillPolys(false);
  //   drawRect(p1, p2);
  // }
}

// ****************************************************************************
void MolDraw2D::drawMolecule(const ROMol &mol, const std::string &legend,
                             const vector<int> *highlight_atoms,
                             const vector<int> *highlight_bonds,
                             const map<int, DrawColour> *highlight_atom_map,
                             const map<int, DrawColour> *highlight_bond_map,
                             const std::map<int, double> *highlight_radii,
                             int confId) {
  if (!legend.empty()) {
    legend_height_ = int(0.05 * double(panelHeight()));
    if (legend_height_ < 20) {
      legend_height_ = 20;
    }
  } else {
    legend_height_ = 0;
  }
  drawMolecule(mol, highlight_atoms, highlight_bonds, highlight_atom_map,
               highlight_bond_map, highlight_radii, confId);
  drawLegend(legend);
}

// ****************************************************************************
void MolDraw2D::drawMoleculeWithHighlights(
    const ROMol &mol, const string &legend,
    const map<int, vector<DrawColour>> &highlight_atom_map,
    const map<int, vector<DrawColour>> &highlight_bond_map,
    const map<int, double> &highlight_radii,
    const map<int, int> &highlight_linewidth_multipliers, int confId) {
  int origWidth = lineWidth();
  vector<int> highlight_atoms;
  for (auto ha : highlight_atom_map) {
    highlight_atoms.emplace_back(ha.first);
  }

  if (!legend.empty()) {
    legend_height_ = int(0.05 * double(panelHeight()));
  } else {
    legend_height_ = 0;
  }
  pushDrawDetails();
  unique_ptr<RWMol> rwmol =
      setupMoleculeDraw(mol, &highlight_atoms, &highlight_radii, confId);
  ROMol const &draw_mol = rwmol ? *(rwmol) : mol;
  if (!draw_mol.getNumConformers()) {
    // clearly, the molecule is in a sorry state.
    return;
  }

  if (!pre_shapes_[activeMolIdx_].empty()) {
    MolDraw2D_detail::drawShapes(*this, pre_shapes_[activeMolIdx_]);
  }

  bool orig_fp = fillPolys();
  setFillPolys(drawOptions().fillHighlights);

  // draw the highlighted bonds first, so the atoms hide the ragged
  // ends.  This only works with filled highlighting, obs.  If not, we need
  // the highlight radii to work out the intersection of the bond highlight
  // with the atom highlight.
  drawHighlightedBonds(draw_mol, highlight_bond_map,
                       highlight_linewidth_multipliers, &highlight_radii);

  for (auto ha : highlight_atom_map) {
    // cout << "highlighting atom " << ha.first << " with " << ha.second.size()
    //      << " colours" << endl;
    drawHighlightedAtom(ha.first, ha.second, &highlight_radii);
  }
  setFillPolys(orig_fp);

  // draw plain bonds on top of highlights.  Use black if either highlight
  // colour is the same as the colour it would have been.
  vector<pair<DrawColour, DrawColour>> bond_colours;
  for (auto bond : draw_mol.bonds()) {
    int beg_at = bond->getBeginAtomIdx();
    DrawColour col1 = getColour(beg_at);
    int end_at = bond->getEndAtomIdx();
    DrawColour col2 = getColour(end_at);
    auto hb = highlight_bond_map.find(bond->getIdx());
    if (hb != highlight_bond_map.end()) {
      const vector<DrawColour> &cols = hb->second;
      if (find(cols.begin(), cols.end(), col1) == cols.end() ||
          find(cols.begin(), cols.end(), col2) == cols.end()) {
        col1 = DrawColour(0.0, 0.0, 0.0);
        col2 = col1;
      }
    }
    bond_colours.emplace_back(make_pair(col1, col2));
  }
  drawBonds(draw_mol, nullptr, nullptr, nullptr, nullptr, &bond_colours);

  vector<DrawColour> atom_colours;
  for (auto this_at : draw_mol.atoms()) {
    // Get colours together for the atom labels.
    // Passing nullptr means that we'll get a colour based on atomic number
    // only.
    atom_colours.emplace_back(getColour(this_at->getIdx(), nullptr, nullptr));
    // if the chosen colour is a highlight colour for this atom, choose black
    // instead so it is still visible.
    auto ha = highlight_atom_map.find(this_at->getIdx());
    if (ha != highlight_atom_map.end()) {
      if (find(ha->second.begin(), ha->second.end(), atom_colours.back()) !=
          ha->second.end()) {
        atom_colours.back() = DrawColour(0.0, 0.0, 0.0);
      }
    }
  }

  // this puts on atom labels and such
  finishMoleculeDraw(draw_mol, atom_colours);
  setLineWidth(origWidth);

  drawLegend(legend);
  popDrawDetails();
}

// ****************************************************************************
void MolDraw2D::get2DCoordsMol(RWMol &mol, double &offset, double spacing,
                               double &maxY, double &minY, int confId,
                               bool shiftAgents, double coordScale) {
  if (drawOptions().prepareMolsBeforeDrawing) {
    mol.updatePropertyCache(false);
    try {
      RDLog::BlockLogs blocker;
      MolOps::Kekulize(mol, false);  // kekulize, but keep the aromatic flags!
    } catch (const MolSanitizeException &) {
      // don't need to do anything
    }
    MolOps::setHybridization(mol);
  }
  if (!mol.getNumConformers()) {
    const bool canonOrient = true;
    RDDepict::compute2DCoords(mol, nullptr, canonOrient);
  } else {
    // we need to center the molecule
    centerMolForDrawing(mol, confId);
  }
  // when preparing a reaction component to be drawn we should neither kekulize
  // (we did that above if required) nor add chiralHs
  const bool kekulize = false;
  const bool addChiralHs = false;
  MolDraw2DUtils::prepareMolForDrawing(mol, kekulize, addChiralHs);
  double minX = 1e8;
  double maxX = -1e8;
  double vShift = 0;
  if (shiftAgents) {
    vShift = 1.1 * maxY / 2;
  }

  pushDrawDetails();

  extractAtomCoords(mol, confId, false);
  extractAtomSymbols(mol);
  for (unsigned int i = 0; i < mol.getNumAtoms(); ++i) {
    RDGeom::Point2D p = at_cds_[activeMolIdx_][i];
    Atom *at = mol.getAtomWithIdx(i);
    // allow for the width of the atom label.
    auto at_lab = getAtomSymbolAndOrientation(*at);
    double width = 0.0, height = 0.0;
    if (!at_lab.first.empty()) {
      getLabelSize(at_lab.first, at_lab.second, width, height);
    }
    if (at_lab.second == OrientType::W) {
      p.x -= width;
    } else {
      p.x -= width / 2;
    }
    p *= coordScale;
    minX = std::min(minX, p.x);
  }
  offset += fabs(minX);
  Conformer &conf = mol.getConformer(confId);
  for (unsigned int i = 0; i < mol.getNumAtoms(); ++i) {
    RDGeom::Point2D p = at_cds_[activeMolIdx_][i];
    p.y = p.y * coordScale + vShift;
    Atom *at = mol.getAtomWithIdx(i);
    // allow for the width of the atom label.
    auto at_lab = getAtomSymbolAndOrientation(*at);
    double width = 0.0, height = 0.0;
    if (!at_lab.first.empty()) {
      getLabelSize(at_lab.first, at_lab.second, width, height);
    }
    height /= 2.0;
    if (at_lab.second != OrientType::E) {
      width /= 2.0;
    }
    if (!shiftAgents) {
      maxY = std::max(p.y + height, maxY);
      minY = std::min(p.y - height, minY);
    }
    p.x = p.x * coordScale + offset;
    maxX = std::max(p.x + width, maxX);

    // now copy the transformed coords back to the actual
    // molecules.  The initial calculations were done on the
    // copies taken by extractAtomCoords, and that was so
    // we could re-use existing code for scaling the picture
    // including labels.
    RDGeom::Point3D &at_cds = conf.getAtomPos(i);
    at_cds.x = p.x;
    at_cds.y = p.y;
  }
  offset = maxX + spacing;
  popDrawDetails();
}

// ****************************************************************************
void MolDraw2D::get2DCoordsForReaction(ChemicalReaction &rxn,
                                       Point2D &arrowBegin, Point2D &arrowEnd,
                                       std::vector<double> &plusLocs,
                                       double spacing,
                                       const std::vector<int> *confIds) {
  plusLocs.resize(0);
  double maxY = -1e8, minY = 1e8;
  double offset = 0.0;

  // reactants
  for (unsigned int midx = 0; midx < rxn.getNumReactantTemplates(); ++midx) {
    // add space for the "+" if required
    if (midx > 0) {
      plusLocs.push_back(offset);
      offset += spacing;
    }
    ROMOL_SPTR reactant = rxn.getReactants()[midx];
    int cid = -1;
    if (confIds) {
      cid = (*confIds)[midx];
    }
    get2DCoordsMol(*(RWMol *)reactant.get(), offset, spacing, maxY, minY, cid,
                   false, 1.0);
  }
  arrowBegin.x = offset;

  offset += spacing;

  double begAgentOffset = offset;

  // we need to do the products now so that we know the full y range.
  // these will have the wrong X coordinates, but we'll fix that later.
  offset = 0;
  for (unsigned int midx = 0; midx < rxn.getNumProductTemplates(); ++midx) {
    // add space for the "+" if required
    if (midx > 0) {
      plusLocs.push_back(offset);
      offset += spacing;
    }
    ROMOL_SPTR product = rxn.getProducts()[midx];
    int cid = -1;
    if (confIds) {
      cid = (*confIds)[rxn.getNumReactantTemplates() +
                       rxn.getNumAgentTemplates() + midx];
    }
    get2DCoordsMol(*(RWMol *)product.get(), offset, spacing, maxY, minY, cid,
                   false, 1.0);
  }

  offset = begAgentOffset;
  // agents
  for (unsigned int midx = 0; midx < rxn.getNumAgentTemplates(); ++midx) {
    ROMOL_SPTR agent = rxn.getAgents()[midx];
    int cid = -1;
    if (confIds) {
      cid = (*confIds)[rxn.getNumReactantTemplates() + midx];
    }
    get2DCoordsMol(*(RWMol *)agent.get(), offset, spacing, maxY, minY, cid,
                   true, 0.45);
  }
  if (rxn.getNumAgentTemplates()) {
    arrowEnd.x = offset;  //- spacing;
  } else {
    arrowEnd.x = offset + 3 * spacing;
  }
  offset = arrowEnd.x + 1.5 * spacing;

  // now translate the products over
  for (unsigned int midx = 0; midx < rxn.getNumProductTemplates(); ++midx) {
    ROMOL_SPTR product = rxn.getProducts()[midx];
    int cid = -1;
    if (confIds) {
      cid = (*confIds)[rxn.getNumReactantTemplates() +
                       rxn.getNumAgentTemplates() + midx];
    }
    Conformer &conf = product->getConformer(cid);
    for (unsigned int aidx = 0; aidx < product->getNumAtoms(); ++aidx) {
      conf.getAtomPos(aidx).x += offset;
    }
  }

  // fix the plus signs too
  unsigned int startP = 0;
  if (rxn.getNumReactantTemplates() > 1) {
    startP = rxn.getNumReactantTemplates() - 1;
  }
  for (unsigned int pidx = startP; pidx < plusLocs.size(); ++pidx) {
    plusLocs[pidx] += offset;
  }

  arrowBegin.y = arrowEnd.y = minY + (maxY - minY) / 2;
}

// ****************************************************************************
void MolDraw2D::drawReaction(
    const ChemicalReaction &rxn, bool highlightByReactant,
    const std::vector<DrawColour> *highlightColorsReactants,
    const std::vector<int> *confIds) {
  ChemicalReaction nrxn(rxn);
  double spacing = 1.0;
  Point2D arrowBegin, arrowEnd;
  std::vector<double> plusLocs;
  get2DCoordsForReaction(nrxn, arrowBegin, arrowEnd, plusLocs, spacing,
                         confIds);

  MolDrawOptions origDrawOptions = drawOptions();
  drawOptions().prepareMolsBeforeDrawing = false;
  drawOptions().includeMetadata = false;

  ROMol *tmol = ChemicalReactionToRxnMol(nrxn);
  MolOps::findSSSR(*tmol);

  if (needs_scale_ &&
      (!nrxn.getNumReactantTemplates() || !nrxn.getNumProductTemplates())) {
    // drawMolecule() will figure out the scaling so that the molecule
    // fits the drawing pane. In order to ensure that we have space for the
    // arrow, we need to figure out the scaling on our own.
    RWMol tmol2;
    tmol2.addAtom(new Atom(0), true, true);
    tmol2.addAtom(new Atom(0), true, true);
    tmol2.addConformer(new Conformer(2), true);
    tmol2.getConformer().getAtomPos(0) =
        RDGeom::Point3D(arrowBegin.x, arrowBegin.y, 0);
    tmol2.getConformer().getAtomPos(1) =
        RDGeom::Point3D(arrowEnd.x, arrowEnd.y, 0);

    for (auto atom : tmol2.atoms()) {
      atom->calcImplicitValence();
    }

    tmol2.insertMol(*tmol);
    pushDrawDetails();
    extractAtomCoords(tmol2, 0, true);
    extractAtomSymbols(tmol2);
    calculateScale(panelWidth(), drawHeight(), tmol2);
    needs_scale_ = false;
    popDrawDetails();
  }

  std::vector<int> *atom_highlights = nullptr;
  std::map<int, DrawColour> *atom_highlight_colors = nullptr;
  std::vector<int> *bond_highlights = nullptr;
  std::map<int, DrawColour> *bond_highlight_colors = nullptr;
  if (highlightByReactant) {
    const std::vector<DrawColour> *colors =
        &drawOptions().highlightColourPalette;
    if (highlightColorsReactants) {
      colors = highlightColorsReactants;
    }
    std::vector<int> atomfragmap;
    MolOps::getMolFrags(*tmol, atomfragmap);

    atom_highlights = new std::vector<int>();
    atom_highlight_colors = new std::map<int, DrawColour>();
    bond_highlights = new std::vector<int>();
    bond_highlight_colors = new std::map<int, DrawColour>();
    std::map<int, int> atommap_fragmap;
    for (unsigned int aidx = 0; aidx < tmol->getNumAtoms(); ++aidx) {
      int atomRole = -1;
      Atom *atom = tmol->getAtomWithIdx(aidx);
      if (atom->getPropIfPresent("molRxnRole", atomRole) && atomRole == 1 &&
          atom->getAtomMapNum()) {
        atommap_fragmap[atom->getAtomMapNum()] = atomfragmap[aidx];
        atom_highlights->push_back(aidx);
        (*atom_highlight_colors)[aidx] =
            (*colors)[atomfragmap[aidx] % colors->size()];

        atom->setAtomMapNum(0);
        // add highlighted bonds to lower-numbered
        // (and thus already covered) neighbors
        for (const auto &nbri :
             make_iterator_range(tmol->getAtomNeighbors(atom))) {
          const Atom *nbr = (*tmol)[nbri];
          if (nbr->getIdx() < aidx &&
              atomfragmap[nbr->getIdx()] == atomfragmap[aidx]) {
            int bondIdx =
                tmol->getBondBetweenAtoms(aidx, nbr->getIdx())->getIdx();
            bond_highlights->push_back(bondIdx);
            (*bond_highlight_colors)[bondIdx] = (*atom_highlight_colors)[aidx];
          }
        }
      }
    }
    for (unsigned int aidx = 0; aidx < tmol->getNumAtoms(); ++aidx) {
      int atomRole = -1;
      Atom *atom = tmol->getAtomWithIdx(aidx);
      if (atom->getPropIfPresent("molRxnRole", atomRole) && atomRole == 2 &&
          atom->getAtomMapNum() &&
          atommap_fragmap.find(atom->getAtomMapNum()) !=
              atommap_fragmap.end()) {
        atom_highlights->push_back(aidx);
        (*atom_highlight_colors)[aidx] =
            (*colors)[atommap_fragmap[atom->getAtomMapNum()] % colors->size()];

        atom->setAtomMapNum(0);
        // add highlighted bonds to lower-numbered
        // (and thus already covered) neighbors
        for (const auto &nbri :
             make_iterator_range(tmol->getAtomNeighbors(atom))) {
          const Atom *nbr = (*tmol)[nbri];
          if (nbr->getIdx() < aidx && (*atom_highlight_colors)[nbr->getIdx()] ==
                                          (*atom_highlight_colors)[aidx]) {
            int bondIdx =
                tmol->getBondBetweenAtoms(aidx, nbr->getIdx())->getIdx();
            bond_highlights->push_back(bondIdx);
            (*bond_highlight_colors)[bondIdx] = (*atom_highlight_colors)[aidx];
          }
        }
      }
    }
  }

  drawMolecule(*tmol, "", atom_highlights, bond_highlights,
               atom_highlight_colors, bond_highlight_colors);

  delete tmol;
  delete atom_highlights;
  delete atom_highlight_colors;
  delete bond_highlights;
  delete bond_highlight_colors;

  double o_font_scale = text_drawer_->fontScale();
  double fsize = text_drawer_->fontSize();
  double new_font_scale =
      2.0 * o_font_scale * drawOptions().legendFontSize / fsize;
  text_drawer_->setFontScale(new_font_scale);

  DrawColour odc = colour();
  setColour(options_.symbolColour);

  // now add the symbols
  for (auto plusLoc : plusLocs) {
    Point2D loc(plusLoc, arrowBegin.y);
    drawString("+", loc);
  }

  // The arrow:
  drawArrow(arrowBegin, arrowEnd);

  if (origDrawOptions.includeMetadata) {
    this->updateMetadata(nrxn);
  }

  setColour(odc);
  text_drawer_->setFontScale(o_font_scale);
  drawOptions() = origDrawOptions;
}

// ****************************************************************************
void MolDraw2D::drawMolecules(
    const std::vector<ROMol *> &mols, const std::vector<std::string> *legends,
    const std::vector<std::vector<int>> *highlight_atoms,
    const std::vector<std::vector<int>> *highlight_bonds,
    const std::vector<std::map<int, DrawColour>> *highlight_atom_maps,
    const std::vector<std::map<int, DrawColour>> *highlight_bond_maps,
    const std::vector<std::map<int, double>> *highlight_radii,
    const std::vector<int> *confIds) {
  PRECONDITION(!legends || legends->size() == mols.size(), "bad size");
  PRECONDITION(!highlight_atoms || highlight_atoms->size() == mols.size(),
               "bad size");
  PRECONDITION(!highlight_bonds || highlight_bonds->size() == mols.size(),
               "bad size");
  PRECONDITION(
      !highlight_atom_maps || highlight_atom_maps->size() == mols.size(),
      "bad size");
  PRECONDITION(
      !highlight_bond_maps || highlight_bond_maps->size() == mols.size(),
      "bad size");
  PRECONDITION(!highlight_radii || highlight_radii->size() == mols.size(),
               "bad size");
  PRECONDITION(!confIds || confIds->size() == mols.size(), "bad size");
  PRECONDITION(panel_width_ != 0, "panel width cannot be zero");
  PRECONDITION(panel_height_ != 0, "panel height cannot be zero");
  if (!mols.size()) {
    return;
  }

  setupTextDrawer();
  vector<unique_ptr<RWMol>> tmols;
  calculateScale(panelWidth(), drawHeight(), mols, highlight_atoms,
                 highlight_radii, confIds, tmols);
  // so drawMolecule doesn't recalculate the scale each time, and
  // undo all the good work.
  needs_scale_ = false;

  int nCols = width() / panelWidth();
  int nRows = height() / panelHeight();
  for (unsigned int i = 0; i < mols.size(); ++i) {
    if (!mols[i]) {
      continue;
    }

    int row = 0;
    // note that this also works when no panel size is specified since
    // the panel dimensions defaults to -1
    if (nRows > 1) {
      row = i / nCols;
    }
    int col = 0;
    if (nCols > 1) {
      col = i % nCols;
    }
    setOffset(col * panelWidth(), row * panelHeight());

    ROMol *draw_mol = tmols[i] ? tmols[i].get() : mols[i];
    unique_ptr<vector<int>> lhighlight_bonds;
    if (highlight_bonds) {
      lhighlight_bonds.reset(new std::vector<int>((*highlight_bonds)[i]));
    } else if (drawOptions().continuousHighlight && highlight_atoms) {
      lhighlight_bonds.reset(new vector<int>());
      getBondHighlightsForAtoms(*draw_mol, (*highlight_atoms)[i],
                                *lhighlight_bonds);
    };

    drawMolecule(*draw_mol, legends ? (*legends)[i] : "",
                 highlight_atoms ? &(*highlight_atoms)[i] : nullptr,
                 lhighlight_bonds.get(),
                 highlight_atom_maps ? &(*highlight_atom_maps)[i] : nullptr,
                 highlight_bond_maps ? &(*highlight_bond_maps)[i] : nullptr,
                 highlight_radii ? &(*highlight_radii)[i] : nullptr,
                 confIds ? (*confIds)[i] : -1);
    // save the drawn positions of the atoms on the molecule. This is the only
    // way that we can later add metadata
    auto tag = boost::str(boost::format("_atomdrawpos_%d") %
                          (confIds ? (*confIds)[i] : -1));
    for (unsigned int j = 0; j < mols[i]->getNumAtoms(); ++j) {
      auto pt = getDrawCoords(j);
      mols[i]->getAtomWithIdx(j)->setProp(tag, pt, true);
    }
  }
}

// ****************************************************************************
void MolDraw2D::highlightCloseContacts() {
  if (drawOptions().flagCloseContactsDist < 0) {
    return;
  }
  int tol =
      drawOptions().flagCloseContactsDist * drawOptions().flagCloseContactsDist;
  boost::dynamic_bitset<> flagged(at_cds_[activeMolIdx_].size());
  for (unsigned int i = 0; i < at_cds_[activeMolIdx_].size(); ++i) {
    if (flagged[i]) {
      continue;
    }
    Point2D ci = getDrawCoords(at_cds_[activeMolIdx_][i]);
    for (unsigned int j = i + 1; j < at_cds_[activeMolIdx_].size(); ++j) {
      if (flagged[j]) {
        continue;
      }
      Point2D cj = getDrawCoords(at_cds_[activeMolIdx_][j]);
      double d = (cj - ci).lengthSq();
      if (d <= tol) {
        flagged.set(i);
        flagged.set(j);
        break;
      }
    }
    if (flagged[i]) {
      Point2D p1 = at_cds_[activeMolIdx_][i];
      Point2D p2 = p1;
      Point2D offset(0.1, 0.1);
      p1 -= offset;
      p2 += offset;
      bool ofp = fillPolys();
      setFillPolys(false);
      DrawColour odc = colour();
      setColour(DrawColour(1, 0, 0));
      drawRect(p1, p2);
      setColour(odc);
      setFillPolys(ofp);
    }
  }
}

// ****************************************************************************
// transform a set of coords in the molecule's coordinate system
// to drawing system coordinates
Point2D MolDraw2D::getDrawCoords(const Point2D &mol_cds) const {
  double x = scale_ * (mol_cds.x - x_min_ + x_trans_);
  double y = scale_ * (mol_cds.y - y_min_ + y_trans_);
  // y is now the distance from the top of the image, we need to
  // invert that:
  x += x_offset_;
  y -= y_offset_;
  y = panelHeight() - legend_height_ - y;
  return Point2D(x, y);
}

// ****************************************************************************
Point2D MolDraw2D::getDrawCoords(int at_num) const {
  PRECONDITION(activeMolIdx_ >= 0, "bad mol idx");
  return getDrawCoords(at_cds_[activeMolIdx_][at_num]);
}

// ****************************************************************************
Point2D MolDraw2D::getAtomCoords(const pair<int, int> &screen_cds) const {
  return getAtomCoords(
      make_pair(double(screen_cds.first), double(screen_cds.second)));
}

Point2D MolDraw2D::getAtomCoords(const pair<double, double> &screen_cds) const {
  double screen_x = screen_cds.first - x_offset_;
  double screen_y = screen_cds.second - y_offset_;
  auto x = double(screen_x / scale_ + x_min_ - x_trans_);
  auto y = double(y_min_ - y_trans_ -
                  (screen_y - panelHeight() + legend_height_) / scale_);
  return Point2D(x, y);
}

// ****************************************************************************
Point2D MolDraw2D::getAtomCoords(int at_num) const {
  PRECONDITION(activeMolIdx_ >= 0, "bad active mol");
  return at_cds_[activeMolIdx_][at_num];
}

// ****************************************************************************
double MolDraw2D::fontSize() const { return text_drawer_->fontSize(); }

// ****************************************************************************
void MolDraw2D::setFontSize(double new_size) {
  text_drawer_->setFontSize(new_size);
}

// ****************************************************************************
void MolDraw2D::setScale(int width, int height, const Point2D &minv,
                         const Point2D &maxv, const ROMol *mol) {
  PRECONDITION(width > 0, "bad width");
  PRECONDITION(height > 0, "bad height");

  double x_max, y_max;
  if (mol) {
    pushDrawDetails();
    unique_ptr<RWMol> tmol =
        setupDrawMolecule(*mol, nullptr, nullptr, -1, width, height);
    calculateScale(height, width, *tmol);
    popDrawDetails();
    x_min_ = min(minv.x, x_min_);
    y_min_ = min(minv.y, y_min_);
    x_max = max(maxv.x, x_range_ + x_min_);
    y_max = max(maxv.y, y_range_ + y_min_);
  } else {
    x_min_ = minv.x;
    y_min_ = minv.y;
    x_max = maxv.x;
    y_max = maxv.y;
  }

  x_range_ = x_max - x_min_;
  y_range_ = y_max - y_min_;

  needs_scale_ = false;

  if (x_range_ < 1.0e-4) {
    x_range_ = 1.0;
    x_min_ = -0.5;
  }
  if (y_range_ < 1.0e-4) {
    y_range_ = 1.0;
    y_min_ = -0.5;
  }

  // put a buffer round the drawing and calculate a final scale
  x_min_ -= drawOptions().padding * x_range_;
  x_range_ *= 1 + 2 * drawOptions().padding;
  y_min_ -= drawOptions().padding * y_range_;
  y_range_ *= 1 + 2 * drawOptions().padding;

  scale_ = std::min(double(width) / x_range_, double(height) / y_range_);
  text_drawer_->setFontScale(scale_);
  double y_mid = y_min_ + 0.5 * y_range_;
  double x_mid = x_min_ + 0.5 * x_range_;
  x_trans_ = y_trans_ = 0.0;  // getDrawCoords uses [xy_]trans_
  Point2D mid = getDrawCoords(Point2D(x_mid, y_mid));
  // that used the offset, we need to remove that:
  mid.x -= x_offset_;
  mid.y += y_offset_;
  x_trans_ = (width / 2 - mid.x) / scale_;
  y_trans_ = (mid.y - height / 2) / scale_;
}

// ****************************************************************************
void MolDraw2D::calculateScale(int width, int height, const ROMol &mol,
                               const std::vector<int> *highlight_atoms,
                               const std::map<int, double> *highlight_radii,
                               int confId) {
  PRECONDITION(width > 0, "bad width");
  PRECONDITION(height > 0, "bad height");
  PRECONDITION(activeMolIdx_ >= 0, "bad active mol");

  // cout << "calculateScale  width = " << width << "  height = " << height
  //      << endl;

  x_min_ = y_min_ = numeric_limits<double>::max();
  double x_max(-x_min_), y_max(-y_min_);

  // first find the bounding box defined by the atoms
  for (const auto &pt : at_cds_[activeMolIdx_]) {
    x_min_ = std::min(pt.x, x_min_);
    y_min_ = std::min(pt.y, y_min_);
    x_max = std::max(pt.x, x_max);
    y_max = std::max(pt.y, y_max);
  }

  // adjust based on the shapes (if any)
  for (const auto &shp : pre_shapes_[activeMolIdx_]) {
    for (const auto &pt : shp.points) {
      x_min_ = std::min(pt.x, x_min_);
      y_min_ = std::min(pt.y, y_min_);
      x_max = std::max(pt.x, x_max);
      y_max = std::max(pt.y, y_max);
    }
  }
  for (const auto &shp : post_shapes_[activeMolIdx_]) {
    for (const auto &pt : shp.points) {
      x_min_ = std::min(pt.x, x_min_);
      y_min_ = std::min(pt.y, y_min_);
      x_max = std::max(pt.x, x_max);
      y_max = std::max(pt.y, y_max);
    }
  }

  // calculate the x and y spans
  x_range_ = x_max - x_min_;
  y_range_ = y_max - y_min_;
  if (x_range_ < 1e-4) {
    x_range_ = 2.0;
    x_min_ -= 1.0;
    x_max += 1.0;
  }
  if (y_range_ < 1e-4) {
    y_range_ = 2.0;
    y_min_ -= 1.0;
    y_max += 1.0;
  }

  scale_ = std::min(double(width) / x_range_, double(height) / y_range_);
  // we may need to adjust the scale if there are atom symbols that go off
  // the edges, and we probably need to do it iteratively because
  // get_string_size uses the current value of scale_.
  // We also need to adjust for highlighted atoms if there are any.
  // And now we need to take account of strings with N/S orientation
  // as well.
  while (scale_ > 1e-4) {
    text_drawer_->setFontScale(scale_);
    adjustScaleForAtomLabels(highlight_atoms, highlight_radii);
    adjustScaleForRadicals(mol);
    if (supportsAnnotations() && !annotations_.empty() &&
        !annotations_[activeMolIdx_].empty()) {
      adjustScaleForAnnotation(annotations_[activeMolIdx_]);
    }
    double old_scale = scale_;
    scale_ = std::min(double(width) / x_range_, double(height) / y_range_);
    if (fabs(scale_ - old_scale) < 0.1) {
      break;
    }
  }

  // put a 5% buffer round the drawing and calculate a final scale
  x_min_ -= drawOptions().padding * x_range_;
  x_range_ *= 1 + 2 * drawOptions().padding;
  y_min_ -= drawOptions().padding * y_range_;
  y_range_ *= 1 + 2 * drawOptions().padding;

  if (x_range_ > 1e-4 || y_range_ > 1e-4) {
    scale_ = std::min(double(width) / x_range_, double(height) / y_range_);
    double fix_scale = scale_;
    // after all that, use the fixed scale unless it's too big, in which case
    // scale the drawing down to fit.
    // fixedScale takes precedence if both it and fixedBondLength are given.
    if (drawOptions().fixedBondLength > 0.0) {
      fix_scale = drawOptions().fixedBondLength;
    }
    if (drawOptions().fixedScale > 0.0) {
      fix_scale = double(width) * drawOptions().fixedScale;
    }
    if (scale_ > fix_scale) {
      scale_ = fix_scale;
    }
    centrePicture(width, height);
  } else {
    scale_ = 1;
    x_trans_ = 0.;
    y_trans_ = 0.;
  }

  const auto &conf = mol.getConformer(confId);
  double meanBondLength = 0.0;
  unsigned int nBonds = 0;
  for (const auto &bond : mol.bonds()) {
    meanBondLength += (conf.getAtomPos(bond->getBeginAtomIdx()) -
                       conf.getAtomPos(bond->getEndAtomIdx()))
                          .length();
    ++nBonds;
  }
  meanBondLength /= nBonds;
  // the rdkit depictor sets bond lengths to be like covalent bond lengths
  // but many others set them to a smaller base value
  // In this case the fonts will be too big, so add a correction there.
  // both the 1.0 and the 0.75 are empirical
  if (meanBondLength < 1.0) {
    text_drawer_->setBaseFontSize(text_drawer_->baseFontSize() * 0.75);
  }
  text_drawer_->setFontScale(scale_);
  // cout << "leaving calculateScale" << endl;
  // cout << "final scale : " << scale_ << endl;
}

// ****************************************************************************
void MolDraw2D::calculateScale(int width, int height,
                               const vector<ROMol *> &mols,
                               const vector<vector<int>> *highlight_atoms,
                               const vector<map<int, double>> *highlight_radii,
                               const vector<int> *confIds,
                               vector<unique_ptr<RWMol>> &tmols) {
  double global_x_min, global_x_max, global_y_min, global_y_max;
  global_x_min = global_y_min = numeric_limits<double>::max();
  global_x_max = global_y_max = -numeric_limits<double>::max();

  double meanBondLength = 0.0;
  unsigned int nBonds = 0;
  for (size_t i = 0; i < mols.size(); ++i) {
    tabulaRasa();
    if (!mols[i]) {
      tmols.emplace_back(unique_ptr<RWMol>(new RWMol));
      continue;
    }
    const vector<int> *ha = highlight_atoms ? &(*highlight_atoms)[i] : nullptr;
    const map<int, double> *hr =
        highlight_radii ? &(*highlight_radii)[i] : nullptr;
    int id = confIds ? (*confIds)[i] : -1;

    pushDrawDetails();
    needs_scale_ = true;
    unique_ptr<RWMol> rwmol =
        setupDrawMolecule(*mols[i], ha, hr, id, width, height);
    double x_max = x_min_ + x_range_;
    double y_max = y_min_ + y_range_;
    global_x_min = min(x_min_, global_x_min);
    global_x_max = max(x_max, global_x_max);
    global_y_min = min(y_min_, global_y_min);
    global_y_max = max(y_max, global_y_max);

    const auto &conf = rwmol->getConformer(id);
    for (const auto &bond : rwmol->bonds()) {
      meanBondLength += (conf.getAtomPos(bond->getBeginAtomIdx()) -
                         conf.getAtomPos(bond->getEndAtomIdx()))
                            .length();
      ++nBonds;
    }

    tmols.emplace_back(std::move(rwmol));
    popDrawDetails();
  }
  meanBondLength /= nBonds;
  // the rdkit depictor sets bond lengths to be like covalent bond lengths
  // but many others set them to a smaller base value
  // In this case the fonts will be too big, so add a correction there.
  // both the 1.0 and the 0.75 are empirical
  if (meanBondLength < 1.0) {
    text_drawer_->setBaseFontSize(text_drawer_->baseFontSize() * 0.75);
  }

  x_min_ = global_x_min;
  y_min_ = global_y_min;
  x_range_ = global_x_max - global_x_min;
  y_range_ = global_y_max - global_y_min;
  scale_ = std::min(double(width) / x_range_, double(height) / y_range_);
  text_drawer_->setFontScale(scale_);
  centrePicture(width, height);
}

// ****************************************************************************
void MolDraw2D::centrePicture(int width, int height) {
  double y_mid = y_min_ + 0.5 * y_range_;
  double x_mid = x_min_ + 0.5 * x_range_;
  Point2D mid;
  // this is getDrawCoords() but using height rather than height()
  // to turn round the y coord and not using x_trans_ and y_trans_
  // which we are trying to calculate at this point.
  mid.x = scale_ * (x_mid - x_min_);
  mid.y = scale_ * (y_mid - y_min_);
  // y is now the distance from the top of the image, we need to
  // invert that:
  mid.x += x_offset_;
  mid.y -= y_offset_;
  mid.y = height - mid.y;

  // that used the offset, we need to remove that:
  mid.x -= x_offset_;
  mid.y += y_offset_;
  x_trans_ = (width / 2 - mid.x) / scale_;
  y_trans_ = (mid.y - height / 2) / scale_;
};

namespace {}  // namespace

// ****************************************************************************
void MolDraw2D::drawLine(const Point2D &cds1, const Point2D &cds2,
                         const DrawColour &col1, const DrawColour &col2) {
  if (drawOptions().comicMode) {
    setFillPolys(false);
    if (col1 == col2) {
      setColour(col1);
      auto pts =
          MolDraw2D_detail::handdrawnLine(cds1, cds2, scale_, true, true);
      drawPolygon(pts);
    } else {
      Point2D mid = (cds1 + cds2) * 0.5;
      setColour(col1);
      auto pts =
          MolDraw2D_detail::handdrawnLine(cds1, mid, scale_, true, false);
      drawPolygon(pts);
      setColour(col2);
      auto pts2 =
          MolDraw2D_detail::handdrawnLine(mid, cds2, scale_, false, true);
      drawPolygon(pts2);
    }
  } else {
    if (col1 == col2) {
      setColour(col1);
      drawLine(cds1, cds2);
    } else {
      Point2D mid = (cds1 + cds2) * 0.5;
      setColour(col1);
      drawLine(cds1, mid);
      setColour(col2);
      drawLine(mid, cds2);
    }
  }
}

// ****************************************************************************
void MolDraw2D::getStringSize(const std::string &label, double &label_width,
                              double &label_height) const {
  text_drawer_->getStringSize(label, label_width, label_height);
  label_width /= scale();
  label_height /= scale();

  // cout << label << " : " << label_width << " by " << label_height
  //     << " : " << scale() << endl;
}

// ****************************************************************************
void MolDraw2D::getLabelSize(const string &label, OrientType orient,
                             double &label_width, double &label_height) const {
  if (orient == OrientType::N || orient == OrientType::S) {
    label_height = 0.0;
    label_width = 0.0;
    vector<string> sym_bits = atomLabelToPieces(label, orient);
    double height, width;
    for (auto bit : sym_bits) {
      getStringSize(bit, width, height);
      if (width > label_width) {
        label_width = width;
      }
      label_height += height;
    }
  } else {
    getStringSize(label, label_width, label_height);
  }
}

// ****************************************************************************
void MolDraw2D::getStringExtremes(const string &label, OrientType orient,
                                  const Point2D &cds, double &x_min,
                                  double &y_min, double &x_max,
                                  double &y_max) const {
  text_drawer_->getStringExtremes(label, orient, x_min, y_min, x_max, y_max);
  Point2D draw_cds = getDrawCoords(cds);
  x_min += draw_cds.x;
  x_max += draw_cds.x;
  y_min += draw_cds.y;
  y_max += draw_cds.y;

  Point2D new_mins = getAtomCoords(make_pair(x_min, y_min));
  Point2D new_maxs = getAtomCoords(make_pair(x_max, y_max));
  x_min = new_mins.x;
  y_min = new_mins.y;
  x_max = new_maxs.x;
  y_max = new_maxs.y;

  // draw coords to atom coords reverses y
  if (y_min > y_max) {
    swap(y_min, y_max);
  }
}

// ****************************************************************************
// draws the string centred on cds
void MolDraw2D::drawString(const string &str, const Point2D &cds) {
  Point2D draw_cds = getDrawCoords(cds);
  text_drawer_->drawString(str, draw_cds, OrientType::N);
  //  int olw = lineWidth();
  //  setLineWidth(0);
  //  text_drawer_->drawStringRects(str, OrientType::N, TextAlignType::MIDDLE,
  //                                draw_cds, *this);
  //  setLineWidth(olw);
}

// ****************************************************************************
void MolDraw2D::drawString(const std::string &str, const Point2D &cds,
                           TextAlignType talign) {
  Point2D draw_cds = getDrawCoords(cds);
  text_drawer_->drawString(str, draw_cds, talign);
}

// ****************************************************************************
DrawColour MolDraw2D::getColour(
    int atom_idx, const std::vector<int> *highlight_atoms,
    const std::map<int, DrawColour> *highlight_map) {
  PRECONDITION(activeMolIdx_ >= 0, "bad mol idx");
  PRECONDITION(atom_idx >= 0, "bad atom_idx");
  PRECONDITION(rdcast<int>(atomic_nums_[activeMolIdx_].size()) > atom_idx,
               "bad atom_idx");
  DrawColour retval =
      getColourByAtomicNum(atomic_nums_[activeMolIdx_][atom_idx]);

  // set contents of highlight_atoms to red
  if (!drawOptions().circleAtoms && !drawOptions().continuousHighlight) {
    if (highlight_atoms &&
        highlight_atoms->end() !=
            find(highlight_atoms->begin(), highlight_atoms->end(), atom_idx)) {
      retval = drawOptions().highlightColour;
    }
    // over-ride with explicit colour from highlight_map if there is one
    if (highlight_map) {
      auto p = highlight_map->find(atom_idx);
      if (p != highlight_map->end()) {
        retval = p->second;
      }
    }
  }
  return retval;
}

// ****************************************************************************
DrawColour MolDraw2D::getColourByAtomicNum(int atomic_num) {
  DrawColour res;
  if (drawOptions().atomColourPalette.find(atomic_num) !=
      drawOptions().atomColourPalette.end()) {
    res = drawOptions().atomColourPalette[atomic_num];
  } else if (atomic_num != -1 && drawOptions().atomColourPalette.find(-1) !=
                                     drawOptions().atomColourPalette.end()) {
    // if -1 is in the palette, we use that for undefined colors
    res = drawOptions().atomColourPalette[-1];
  } else {
    // if all else fails, default to black:
    res = DrawColour(0, 0, 0);
  }
  return res;
}

// ****************************************************************************
unique_ptr<RWMol> MolDraw2D::setupDrawMolecule(
    const ROMol &mol, const vector<int> *highlight_atoms,
    const map<int, double> *highlight_radii, int confId, int width,
    int height) {

  // some of the code in here, such as extractSGroupData requires
  // that everything be working in original coords.  drawMolecules()
  // passes through setupDrawMolecule twice, once to set the global
  // scale, then to actually do the drawing.  It's essential that
  // all the drawing scaling is set to initial values for this, so
  // save the current values before resetting them.  This is relevant
  // principally for when drawMolecules sets the global scale.
  double curr_scale = scale_;
  scale_ = 1.0;
  double curr_font_scale = text_drawer_->fontScale();
  text_drawer_->setFontScale(1.0, true);
  double curr_x_trans = x_trans_;
  double curr_y_trans = y_trans_;
  int curr_x_offset = x_offset_;
  int curr_y_offset = y_offset_;
  double curr_x_min = x_min_;
  double curr_y_min = y_min_;

  x_trans_ = y_trans_ = 0.0;
  x_offset_ = y_offset_ = 0;
  x_min_ = y_min_ = 0.0;

  unique_ptr<RWMol> rwmol{new RWMol(mol)};
  if (drawOptions().prepareMolsBeforeDrawing || !mol.getNumConformers()) {
    MolDraw2DUtils::prepareMolForDrawing(*rwmol);
  }
  if (drawOptions().centreMoleculesBeforeDrawing) {
    if (rwmol->getNumConformers()) {
      centerMolForDrawing(*rwmol, confId);
    }
  }
  if (drawOptions().simplifiedStereoGroupLabel &&
      !mol.hasProp(common_properties::molNote)) {
    auto sgs = mol.getStereoGroups();
    if (sgs.size() == 1) {
      boost::dynamic_bitset<> chiralAts(mol.getNumAtoms());
      for (const auto atom : mol.atoms()) {
        if (atom->getChiralTag() > Atom::ChiralType::CHI_UNSPECIFIED &&
            atom->getChiralTag() < Atom::ChiralType::CHI_OTHER) {
          chiralAts.set(atom->getIdx(), 1);
        }
      }
      for (const auto atm : sgs[0].getAtoms()) {
        chiralAts.set(atm->getIdx(), 0);
      }
      if (chiralAts.none()) {
        // all specified chiral centers are accounted for by this StereoGroup.
        if (sgs[0].getGroupType() == StereoGroupType::STEREO_OR ||
            sgs[0].getGroupType() == StereoGroupType::STEREO_AND) {
          std::vector<StereoGroup> empty;
          rwmol->setStereoGroups(std::move(empty));
          std::string label =
              sgs[0].getGroupType() == StereoGroupType::STEREO_OR
                  ? "OR enantiomer"
                  : "AND enantiomer";
          rwmol->setProp(common_properties::molNote, label);
        }
        // clear the chiral codes on the atoms so that we don't
        // inadvertently draw them later
        for (const auto atm : sgs[0].getAtoms()) {
          rwmol->getAtomWithIdx(atm->getIdx())
              ->clearProp(common_properties::_CIPCode);
        }
      }
    }
  }
  if (!rwmol->getNumConformers()) {
    // clearly, the molecule is in a sorry state.
    return rwmol;
  }

  if (drawOptions().addStereoAnnotation) {
    MolDraw2D_detail::addStereoAnnotation(*rwmol);
  }
  if (drawOptions().addAtomIndices) {
    MolDraw2D_detail::addAtomIndices(*rwmol);
  }
  if (drawOptions().addBondIndices) {
    MolDraw2D_detail::addBondIndices(*rwmol);
  }
  if (!activeMolIdx_) {
    if (drawOptions().clearBackground) {
      clearDrawing();
    }
  }
  bool updateBBox = !activeMolIdx_;
  extractAtomCoords(*rwmol, confId, updateBBox);
  extractAtomSymbols(*rwmol);
  extractAtomNotes(*rwmol);
  extractBondNotes(*rwmol);
  extractRadicals(*rwmol);
  if (activeMolIdx_ >= 0 &&
      post_shapes_.size() > static_cast<size_t>(activeMolIdx_) &&
      pre_shapes_.size() > static_cast<size_t>(activeMolIdx_)) {
    post_shapes_[activeMolIdx_].clear();
    pre_shapes_[activeMolIdx_].clear();
  }
  extractSGroupData(*rwmol);
  extractVariableBonds(*rwmol);
  extractBrackets(*rwmol);
  extractMolNotes(*rwmol);
  extractLinkNodes(*rwmol);

  // set everything to as it was before.
  scale_ = curr_scale;
  text_drawer_->setFontScale(curr_font_scale, true);
  x_trans_ = curr_x_trans;
  y_trans_ = curr_y_trans;
  x_offset_ = curr_x_offset;
  y_offset_ = curr_y_offset;
  x_min_ = curr_x_min;
  y_min_ = curr_y_min;

  if (!activeMolIdx_ && needs_scale_) {
    calculateScale(width, height, *rwmol, highlight_atoms, highlight_radii,
                   confId);
    needs_scale_ = false;
  }

  return rwmol;
}

// ****************************************************************************
void MolDraw2D::pushDrawDetails() {
  at_cds_.push_back(std::vector<Point2D>());
  atomic_nums_.push_back(std::vector<int>());
  atom_syms_.push_back(std::vector<std::pair<std::string, OrientType>>());
  annotations_.push_back(std::vector<AnnotationType>());
  pre_shapes_.push_back(std::vector<MolDrawShape>());
  post_shapes_.push_back(std::vector<MolDrawShape>());
  radicals_.push_back(
      std::vector<std::pair<std::shared_ptr<StringRect>, OrientType>>());
  activeMolIdx_++;
}

// ****************************************************************************
void MolDraw2D::popDrawDetails() {
  activeMolIdx_--;
  annotations_.pop_back();
  pre_shapes_.pop_back();
  post_shapes_.pop_back();
  atom_syms_.pop_back();
  atomic_nums_.pop_back();
  radicals_.pop_back();
  at_cds_.pop_back();
}

// ****************************************************************************
unique_ptr<RWMol> MolDraw2D::setupMoleculeDraw(
    const ROMol &mol, const vector<int> *highlight_atoms,
    const map<int, double> *highlight_radii, int confId) {

  unique_ptr<RWMol> rwmol =
      setupDrawMolecule(mol, highlight_atoms, highlight_radii, confId,
                        panelWidth(), drawHeight());
  ROMol const &draw_mol = rwmol ? *(rwmol) : mol;

  if (drawOptions().includeAtomTags) {
    tagAtoms(draw_mol);
  }
  if (drawOptions().atomRegions.size()) {
    for (const std::vector<int> &region : drawOptions().atomRegions) {
      if (region.size() > 1) {
        Point2D minv = at_cds_[activeMolIdx_][region[0]];
        Point2D maxv = at_cds_[activeMolIdx_][region[0]];
        for (int idx : region) {
          const Point2D &pt = at_cds_[activeMolIdx_][idx];
          minv.x = std::min(minv.x, pt.x);
          minv.y = std::min(minv.y, pt.y);
          maxv.x = std::max(maxv.x, pt.x);
          maxv.y = std::max(maxv.y, pt.y);
        }
        Point2D center = (maxv + minv) / 2;
        Point2D size = (maxv - minv);
        size *= 0.2;
        minv -= size / 2;
        maxv += size / 2;
        setColour(DrawColour(.8, .8, .8));
        // drawEllipse(minv,maxv);
        drawRect(minv, maxv);
      }
    }
  }

  return rwmol;
}

// ****************************************************************************
void MolDraw2D::setupTextDrawer() {
  text_drawer_->setMaxFontSize(drawOptions().maxFontSize);
  text_drawer_->setMinFontSize(drawOptions().minFontSize);
  try {
    text_drawer_->setFontFile(drawOptions().fontFile);
  } catch (std::runtime_error &e) {
    BOOST_LOG(rdWarningLog) << e.what() << std::endl;
    text_drawer_->setFontFile("");
    BOOST_LOG(rdWarningLog) << "Falling back to original font file "
                            << text_drawer_->getFontFile() << "." << std::endl;
  }
}

// ****************************************************************************
void MolDraw2D::drawBonds(
    const ROMol &draw_mol, const vector<int> *highlight_atoms,
    const map<int, DrawColour> *highlight_atom_map,
    const vector<int> *highlight_bonds,
    const map<int, DrawColour> *highlight_bond_map,
    const std::vector<std::pair<DrawColour, DrawColour>> *bond_colours) {
  for (auto this_at : draw_mol.atoms()) {
    int this_idx = this_at->getIdx();
    for (const auto &nbri :
         make_iterator_range(draw_mol.getAtomBonds(this_at))) {
      const Bond *bond = draw_mol[nbri];
      int nbr_idx = bond->getOtherAtomIdx(this_idx);
      if (nbr_idx < static_cast<int>(at_cds_[activeMolIdx_].size()) &&
          nbr_idx > this_idx) {
        drawBond(draw_mol, bond, this_idx, nbr_idx, highlight_atoms,
                 highlight_atom_map, highlight_bonds, highlight_bond_map,
                 bond_colours);
      }
    }
  }
}

// ****************************************************************************
void MolDraw2D::finishMoleculeDraw(const RDKit::ROMol &draw_mol,
                                   const vector<DrawColour> &atom_colours) {
  if (drawOptions().dummiesAreAttachments) {
    for (auto at1 : draw_mol.atoms()) {
      if (at1->hasProp(common_properties::atomLabel) ||
          drawOptions().atomLabels.find(at1->getIdx()) !=
              drawOptions().atomLabels.end()) {
        // skip dummies that explicitly have a label provided
        continue;
      }
      if (at1->getAtomicNum() == 0 && at1->getDegree() == 1) {
        Point2D &at1_cds = at_cds_[activeMolIdx_][at1->getIdx()];
        const auto &iter_pair = draw_mol.getAtomNeighbors(at1);
        const Atom *at2 = draw_mol[*iter_pair.first];
        Point2D &at2_cds = at_cds_[activeMolIdx_][at2->getIdx()];
        drawAttachmentLine(at2_cds, at1_cds, DrawColour(.5, .5, .5));
      }
    }
  }

  for (int i = 0, is = atom_syms_[activeMolIdx_].size(); i < is; ++i) {
    if (!atom_syms_[activeMolIdx_][i].first.empty()) {
      drawAtomLabel(i, atom_colours[i]);
    }
  }
  text_drawer_->setColour(drawOptions().annotationColour);
  if (!supportsAnnotations() && !annotations_.empty()) {
    BOOST_LOG(rdWarningLog) << "annotations not currently supported for this "
                               "MolDraw2D class, they will be ignored."
                            << std::endl;
  } else {
    for (const auto &annotation : annotations_[activeMolIdx_]) {
      drawAnnotation(annotation);
    }
  }

  if (drawOptions().includeRadicals) {
    drawRadicals(draw_mol);
  }

  if (!post_shapes_[activeMolIdx_].empty()) {
    MolDraw2D_detail::drawShapes(*this, post_shapes_[activeMolIdx_]);
  }

  if (drawOptions().flagCloseContactsDist >= 0) {
    highlightCloseContacts();
  }
}

// ****************************************************************************
void MolDraw2D::drawLegend(const string &legend) {
  int olh = legend_height_;
  legend_height_ = 0;  // so we use the whole panel

  auto calc_legend_height = [&](const std::vector<std::string> &legend_bits,
                                double &total_width, double &total_height) {
    total_width = total_height = 0;
    for (auto bit : legend_bits) {
      double x_min, y_min, x_max, y_max;
      text_drawer_->getStringExtremes(bit, OrientType::N, x_min, y_min, x_max,
                                      y_max, true);
      total_height += y_max - y_min;
      total_width = std::max(total_width, x_max - x_min);
    }
  };

  if (!legend.empty()) {
    std::vector<std::string> legend_bits;
    // split any strings on newlines
    string next_piece;
    for (auto c : legend) {
      if (c == '\n') {
        if (!next_piece.empty()) {
          legend_bits.push_back(next_piece);
        }
        next_piece = "";
      } else {
        next_piece += c;
      }
    }
    if (!next_piece.empty()) {
      legend_bits.push_back(next_piece);
    }

    double o_font_scale = text_drawer_->fontScale();
    double fsize = text_drawer_->fontSize();
    double new_font_scale = o_font_scale * drawOptions().legendFontSize / fsize;
    text_drawer_->setFontScale(new_font_scale, true);
    double total_width, total_height;
    calc_legend_height(legend_bits, total_width, total_height);
    if (total_height > olh) {
      new_font_scale *= double(olh) / total_height;
      text_drawer_->setFontScale(new_font_scale, true);
      calc_legend_height(legend_bits, total_width, total_height);
    }
    if (total_width > panelWidth()) {
      new_font_scale *= double(panelWidth()) / total_width;
      text_drawer_->setFontScale(new_font_scale, true);
      calc_legend_height(legend_bits, total_width, total_height);
    }

    text_drawer_->setColour(drawOptions().legendColour);
    Point2D loc(x_offset_ + panelWidth() / 2,
                y_offset_ + panelHeight() - total_height);
    for (auto bit : legend_bits) {
      text_drawer_->drawString(bit, loc, TextAlignType::MIDDLE);
      double x_min, y_min, x_max, y_max;
      text_drawer_->getStringExtremes(bit, OrientType::N, x_min, y_min, x_max,
                                      y_max, true);
      loc.y += y_max - y_min;
    }
    text_drawer_->setFontScale(o_font_scale, true);
  }

  legend_height_ = olh;
}

// ****************************************************************************
void MolDraw2D::drawHighlightedAtom(int atom_idx,
                                    const vector<DrawColour> &colours,
                                    const map<int, double> *highlight_radii) {
  double xradius, yradius;
  Point2D centre;

  calcLabelEllipse(atom_idx, highlight_radii, centre, xradius, yradius);

  int orig_lw = lineWidth();
  bool orig_fp = fillPolys();
  if (!drawOptions().fillHighlights) {
    setLineWidth(getHighlightBondWidth(-1, nullptr));
    setFillPolys(false);
  } else {
    setFillPolys(true);
  }
  if (colours.size() == 1) {
    setColour(colours.front());
    Point2D offset(xradius, yradius);
    Point2D p1 = centre - offset;
    Point2D p2 = centre + offset;
    if (fillPolys()) {
      setLineWidth(1);
    }
    drawEllipse(p1, p2);

    // drawArc(centre, xradius, yradius, 0.0, 360.0);
  } else {
    double arc_size = 360.0 / double(colours.size());
    double arc_start = -90.0;
    for (size_t i = 0; i < colours.size(); ++i) {
      setColour(colours[i]);
      drawArc(centre, xradius, yradius, arc_start, arc_start + arc_size);
      arc_start += arc_size;
    }
  }

  setFillPolys(orig_fp);
  setLineWidth(orig_lw);
}

// ****************************************************************************
void MolDraw2D::calcLabelEllipse(int atom_idx,
                                 const map<int, double> *highlight_radii,
                                 Point2D &centre, double &xradius,
                                 double &yradius) const {
  centre = at_cds_[activeMolIdx_][atom_idx];
  xradius = drawOptions().highlightRadius;
  yradius = xradius;
  if (highlight_radii &&
      highlight_radii->find(atom_idx) != highlight_radii->end()) {
    xradius = highlight_radii->find(atom_idx)->second;
    yradius = xradius;
  }

  if (drawOptions().atomHighlightsAreCircles ||
      atom_syms_[activeMolIdx_][atom_idx].first.empty()) {
    return;
  }

  string atsym = atom_syms_[activeMolIdx_][atom_idx].first;
  OrientType orient = atom_syms_[activeMolIdx_][atom_idx].second;
  double x_min, y_min, x_max, y_max;
  getStringExtremes(atsym, orient, centre, x_min, y_min, x_max, y_max);

  static const double root_2 = sqrt(2.0);
  xradius = max(xradius, root_2 * 0.5 * (x_max - x_min));
  yradius = max(yradius, root_2 * 0.5 * (y_max - y_min));
  centre.x = 0.5 * (x_max + x_min);
  centre.y = 0.5 * (y_max + y_min);
}

// ****************************************************************************
void MolDraw2D::calcAnnotationPosition(const ROMol &,
                                       AnnotationType &annot) const {

  if (annot.text_.empty()) {
    annot.rect_.width_ = -1.0;  // so we know it's not valid.
    return;
  }

  vector<std::shared_ptr<StringRect>> rects;
  vector<TextDrawType> draw_modes;
  vector<char> draw_chars;

  // at this point, the scale() should still be 1, so min and max font sizes
  // don't make sense, as we're effectively operating on atom coords rather
  // than draw.
  double full_font_scale = text_drawer_->fontScale();
  text_drawer_->setFontScale(1, true);
  text_drawer_->getStringRects(annot.text_, OrientType::N, rects, draw_modes,
                               draw_chars);
  text_drawer_->setFontScale(full_font_scale, true);
  // accumulate the widths of the rectangles so that we have the overall width
  for (const auto &rect : rects) {
    annot.rect_.width_ += rect->width_;
  }

  Point2D centroid{0., 0.};
  Point2D minPt{100000., 100000.};
  Point2D maxPt{-100000., -100000.};
  for (const auto &pt : at_cds_[activeMolIdx_]) {
    centroid += pt;
    minPt.x = std::min(pt.x, minPt.x);
    minPt.y = std::min(pt.y, minPt.y);
    maxPt.x = std::max(pt.x, maxPt.x);
    maxPt.y = std::max(pt.y, maxPt.y);
  }
  centroid /= at_cds_[activeMolIdx_].size();

  auto vect = maxPt - centroid;
  auto loc = centroid + vect * 0.9;
  annot.rect_.trans_ = loc;

}

// ****************************************************************************
void MolDraw2D::calcAnnotationPosition(const ROMol &mol, const Atom *atom,
                                       AnnotationType &annot) const {
  PRECONDITION(atom, "no atom");
  if (annot.text_.empty()) {
    annot.rect_.width_ = -1.0;  // so we know it's not valid.
    return;
  }

  Point2D const &at_cds = at_cds_[activeMolIdx_][atom->getIdx()];
  annot.rect_.trans_.x = at_cds.x;
  annot.rect_.trans_.y = at_cds.y;
  double start_ang = getNoteStartAngle(mol, atom);
  calcAtomAnnotationPosition(mol, atom, start_ang, annot);

}

// ****************************************************************************
void MolDraw2D::calcAnnotationPosition(const ROMol &mol, const Bond *bond,
                                       AnnotationType &annot) const {
  PRECONDITION(bond, "no bond");
  if (annot.text_.empty()) {
    annot.rect_.width_ = -1.0;  // so we know it's not valid.
  }
  vector<std::shared_ptr<StringRect>> rects;
  vector<TextDrawType> draw_modes;
  vector<char> draw_chars;

  // at this point, the scale() should still be 1, so min and max font sizes
  // don't make sense, as we're effectively operating on atom coords rather
  // than draw.
  double full_font_scale = text_drawer_->fontScale();
  text_drawer_->setFontScale(drawOptions().annotationFontScale, true);
  text_drawer_->getStringRects(annot.text_, OrientType::N, rects, draw_modes,
                               draw_chars);
  text_drawer_->setFontScale(full_font_scale, true);

  Point2D const &at1_cds = at_cds_[activeMolIdx_][bond->getBeginAtomIdx()];
  Point2D const &at2_cds = at_cds_[activeMolIdx_][bond->getEndAtomIdx()];
  Point2D perp = calcPerpendicular(at1_cds, at2_cds);
  Point2D bond_vec = at1_cds.directionVector(at2_cds);
  double bond_len = (at1_cds - at2_cds).length();
  vector<double> mid_offsets{0.5, 0.33, 0.66, 0.25, 0.75};
  double offset_step = drawOptions().multipleBondOffset;
  StringRect least_worst_rect = StringRect();
  least_worst_rect.clash_score_ = 100;
  for (auto mo : mid_offsets) {
    Point2D mid = at1_cds + bond_vec * bond_len * mo;
    for (int j = 1; j < 6; ++j) {
      if (j == 1 && bond->getBondType() > 1) {
        continue;  // multiple bonds will need a bigger offset.
      }
      double offset = j * offset_step;
      annot.rect_.trans_ = mid + perp * offset;
      StringRect tr(annot.rect_);
      Point2D note_pos = getAtomCoords(make_pair(annot.rect_.trans_.x, annot.rect_.trans_.y));
      int clash_score = doesBondNoteClash(note_pos, rects, mol, bond);
      if (!clash_score) {
        return;
      }
      if (clash_score < least_worst_rect.clash_score_) {
        least_worst_rect = annot.rect_;
      }
      note_pos = mid - perp * offset;
      annot.rect_.trans_ = mid - perp * offset;
      note_pos = getAtomCoords(make_pair(note_pos.x, note_pos.y));
      clash_score = doesBondNoteClash(note_pos, rects, mol, bond);
      if (!clash_score) {
        return;
      }
      if (clash_score < least_worst_rect.clash_score_) {
        least_worst_rect = annot.rect_;
      }
    }
  }

}

// ****************************************************************************
void MolDraw2D::calcAtomAnnotationPosition(const ROMol &mol, const Atom *atom,
                                           double start_ang,
                                           AnnotationType &annot) const {

  Point2D const &at_cds = at_cds_[activeMolIdx_][atom->getIdx()];
  auto const &atsym = atom_syms_[activeMolIdx_][atom->getIdx()];

  vector<std::shared_ptr<StringRect>> rects;
  vector<TextDrawType> draw_modes;
  vector<char> draw_chars;

  // at this point, the scale() should still be 1, so min and max font sizes
  // don't make sense, as we're effectively operating on atom coords rather
  // than draw.
  double full_font_scale = text_drawer_->fontScale();
  text_drawer_->setFontScale(drawOptions().annotationFontScale, true);
  text_drawer_->getStringRects(annot.text_, OrientType::C, rects, draw_modes,
                               draw_chars, false, annot.align_);
  text_drawer_->setFontScale(full_font_scale, true);

  double rad_step = 0.25;
  StringRect least_worst_rect = StringRect();
  least_worst_rect.clash_score_ = 100;
  for (int j = 1; j < 4; ++j) {
    double note_rad = j * rad_step;
    // experience suggests if there's an atom symbol, the close in
    // radius won't work.
    if (j == 1 && !atsym.first.empty()) {
      continue;
    }
    // scan at 30 degree intervals around the atom looking for somewhere
    // clear for the annotation.
    for (int i = 0; i < 12; ++i) {
      double ang = start_ang + i * 30.0 * M_PI / 180.0;
      annot.rect_.trans_.x = at_cds.x + cos(ang) * note_rad;
      annot.rect_.trans_.y = at_cds.y + sin(ang) * note_rad;
      Point2D note_pos = getAtomCoords(make_pair(annot.rect_.trans_.x, annot.rect_.trans_.y));
      int clash_score = doesAtomNoteClash(note_pos, rects, mol, atom->getIdx());
      if (!clash_score) {
        return;
      } else {
        if (clash_score < least_worst_rect.clash_score_) {
          least_worst_rect = annot.rect_;
        }
      }
    }
  }
  annot.rect_ = least_worst_rect;
}

// ****************************************************************************
void MolDraw2D::drawHighlightedBonds(
    const RDKit::ROMol &mol,
    const map<int, vector<DrawColour>> &highlight_bond_map,
    const map<int, int> &highlight_linewidth_multipliers,
    const map<int, double> *highlight_radii) {
  int orig_lw = lineWidth();
  for (auto hb : highlight_bond_map) {
    int bond_idx = hb.first;
    if (!drawOptions().fillHighlights) {
      setLineWidth(
          getHighlightBondWidth(bond_idx, &highlight_linewidth_multipliers));
    }
    auto bond = mol.getBondWithIdx(bond_idx);
    int at1_idx = bond->getBeginAtomIdx();
    int at2_idx = bond->getEndAtomIdx();
    Point2D at1_cds = at_cds_[activeMolIdx_][at1_idx];
    Point2D at2_cds = at_cds_[activeMolIdx_][at2_idx];
    Point2D perp = calcPerpendicular(at1_cds, at2_cds);
    double rad = 0.7 * drawOptions().highlightRadius;
    auto draw_adjusted_line = [&](Point2D p1, Point2D p2) {
      adjustLineEndForHighlight(at1_idx, highlight_radii, p2, p1);
      adjustLineEndForHighlight(at2_idx, highlight_radii, p1, p2);
      bool orig_lws = drawOptions().scaleBondWidth;
      drawOptions().scaleBondWidth = drawOptions().scaleHighlightBondWidth;
      drawLine(p1, p2);
      drawOptions().scaleBondWidth = orig_lws;
    };

    if (hb.second.size() < 2) {
      DrawColour col;
      if (hb.second.empty()) {
        col = drawOptions().highlightColour;
      } else {
        col = hb.second.front();
      }
      setColour(col);
      if (drawOptions().fillHighlights) {
        vector<Point2D> line_pts;
        line_pts.emplace_back(at1_cds + perp * rad);
        line_pts.emplace_back(at2_cds + perp * rad);
        line_pts.emplace_back(at2_cds - perp * rad);
        line_pts.emplace_back(at1_cds - perp * rad);
        drawPolygon(line_pts);
      } else {
        draw_adjusted_line(at1_cds + perp * rad, at2_cds + perp * rad);
        draw_adjusted_line(at1_cds - perp * rad, at2_cds - perp * rad);
      }
    } else {
      double col_rad = 2.0 * rad / hb.second.size();
      if (drawOptions().fillHighlights) {
        Point2D p1 = at1_cds - perp * rad;
        Point2D p2 = at2_cds - perp * rad;
        vector<Point2D> line_pts;
        for (size_t i = 0; i < hb.second.size(); ++i) {
          setColour(hb.second[i]);
          line_pts.clear();
          line_pts.emplace_back(p1);
          line_pts.emplace_back(p1 + perp * col_rad);
          line_pts.emplace_back(p2 + perp * col_rad);
          line_pts.emplace_back(p2);
          drawPolygon(line_pts);
          p1 += perp * col_rad;
          p2 += perp * col_rad;
        }
      } else {
        int step = 0;
        for (size_t i = 0; i < hb.second.size(); ++i) {
          setColour(hb.second[i]);
          // draw even numbers from the bottom, odd from the top
          Point2D offset = perp * (rad - step * col_rad);
          if (!(i % 2)) {
            draw_adjusted_line(at1_cds - offset, at2_cds - offset);
          } else {
            draw_adjusted_line(at1_cds + offset, at2_cds + offset);
            step++;
          }
        }
      }
    }
    setLineWidth(orig_lw);
  }
}

// ****************************************************************************
int MolDraw2D::getHighlightBondWidth(
    int bond_idx, const map<int, int> *highlight_linewidth_multipliers) const {
  int bwm = drawOptions().highlightBondWidthMultiplier;
  // if we're not doing filled highlights, the lines need to be narrower
  if (!drawOptions().fillHighlights) {
    bwm /= 2;
    if (bwm < 1) {
      bwm = 1;
    }
  }

  if (highlight_linewidth_multipliers &&
      !highlight_linewidth_multipliers->empty()) {
    auto it = highlight_linewidth_multipliers->find(bond_idx);
    if (it != highlight_linewidth_multipliers->end()) {
      bwm = it->second;
    }
  }
  int tgt_lw = lineWidth() * bwm;
  return tgt_lw;
}

// ****************************************************************************
void MolDraw2D::adjustLineEndForHighlight(
    int at_idx, const map<int, double> *highlight_radii, Point2D p1,
    Point2D &p2) const {
  // this code is transliterated from
  // http://csharphelper.com/blog/2017/08/calculate-where-a-line-segment-and-an-ellipse-intersect-in-c/
  // which has it in C#
  double xradius, yradius;
  Point2D centre;
  calcLabelEllipse(at_idx, highlight_radii, centre, xradius, yradius);
  // cout << "ellipse is : " << centre.x << ", " << centre.y << " rads " <<
  // xradius << " and " << yradius << endl; cout << "p1 = " << p1.x << ", " <<
  // p1.y << endl << "p2 = " << p2.x << ", " << p2.y << endl;
  if (xradius < 1.0e-6 || yradius < 1.0e-6) {
    return;
  }

  // move everything so the ellipse is centred on the origin.
  p1 -= centre;
  p2 -= centre;
  double a2 = xradius * xradius;
  double b2 = yradius * yradius;
  double A =
      (p2.x - p1.x) * (p2.x - p1.x) / a2 + (p2.y - p1.y) * (p2.y - p1.y) / b2;
  double B = 2.0 * p1.x * (p2.x - p1.x) / a2 + 2.0 * p1.y * (p2.y - p1.y) / b2;
  double C = p1.x * p1.x / a2 + p1.y * p1.y / b2 - 1.0;

  auto t_to_point = [&](double t) -> Point2D {
    Point2D ret_val;
    ret_val.x = p1.x + (p2.x - p1.x) * t + centre.x;
    ret_val.y = p1.y + (p2.y - p1.y) * t + centre.y;
    return ret_val;
  };

  double disc = B * B - 4.0 * A * C;
  if (disc < 0.0) {
    // no solutions, leave things as they are.  Bit crap, though.
    return;
  } else if (fabs(disc) < 1.0e-6) {
    // 1 solution
    double t = -B / (2.0 * A);
    // cout << "t = " << t << endl;
    p2 = t_to_point(t);
  } else {
    // 2 solutions - take the one nearest p1.
    double disc_rt = sqrt(disc);
    double t1 = (-B + disc_rt) / (2.0 * A);
    double t2 = (-B - disc_rt) / (2.0 * A);
    // cout << "t1 = " << t1 << "  t2 = " << t2 << endl;
    double t;
    // prefer the t between 0 and 1, as that must be between the original
    // points.  If both are, prefer the lower, as that will be nearest p1,
    // so on the bit of the ellipse the line comes to first.
    bool t1_ok = (t1 >= 0.0 && t1 <= 1.0);
    bool t2_ok = (t2 >= 0.0 && t2 <= 1.0);
    if (t1_ok && !t2_ok) {
      t = t1;
    } else if (t2_ok && !t1_ok) {
      t = t2;
    } else if (t1_ok && t2_ok) {
      t = min(t1, t2);
    } else {
      // the intersections are both outside the line between p1 and p2
      // so don't do anything.
      return;
    }
    // cout << "using t = " << t << endl;
    p2 = t_to_point(t);
  }
  // cout << "p2 = " << p2.x << ", " << p2.y << endl;
}

// ****************************************************************************
void MolDraw2D::extractAtomCoords(const ROMol &mol, int confId,
                                  bool updateBBox) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(at_cds_.size()) > activeMolIdx_, "no space");
  PRECONDITION(static_cast<int>(atomic_nums_.size()) > activeMolIdx_,
               "no space");
  PRECONDITION(static_cast<int>(mol.getNumConformers()) > 0, "no coords");

  if (updateBBox) {
    bbox_[0].x = bbox_[0].y = numeric_limits<double>::max();
    bbox_[1].x = bbox_[1].y = -1 * numeric_limits<double>::max();
  }
  const RDGeom::POINT3D_VECT &locs = mol.getConformer(confId).getPositions();

  // the transformation rotates anti-clockwise, as is conventional, but
  // probably not what our user expects.
  double rot = -drawOptions().rotate * M_PI / 180.0;
  // assuming that if drawOptions().rotate is set to 0.0, rot will be
  // exactly 0.0 without worrying about floating point number dust.  Does
  // anyone know if this is true?  It's not the end of the world if not,
  // as it's just an extra largely pointless rotation.
  // Floating point numbers are like piles of sand; every time you move
  // them around, you lose a little sand and pick up a little dirt.
  // — Brian Kernighan and P.J. Plauger
  // Nothing brings fear to my heart more than a floating point number.
  // — Gerald Jay Sussman
  // Some developers, when encountering a problem, say: “I know, I’ll
  // use floating-point numbers!”   Now, they have 1.9999999997 problems.
  // — unknown
  RDGeom::Transform2D trans;
  trans.SetTransform(Point2D(0.0, 0.0), rot);
  at_cds_[activeMolIdx_].clear();
  for (auto this_at : mol.atoms()) {
    int this_idx = this_at->getIdx();
    Point2D pt(locs[this_idx].x, locs[this_idx].y);
    if (rot != 0.0) {
      trans.TransformPoint(pt);
    }
    at_cds_[activeMolIdx_].emplace_back(pt);

    if (updateBBox) {
      bbox_[0].x = std::min(bbox_[0].x, pt.x);
      bbox_[0].y = std::min(bbox_[0].y, pt.y);
      bbox_[1].x = std::max(bbox_[1].x, pt.x);
      bbox_[1].y = std::max(bbox_[1].y, pt.y);
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractAtomSymbols(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(atom_syms_.size()) > activeMolIdx_, "no space");
  PRECONDITION(static_cast<int>(atomic_nums_.size()) > activeMolIdx_,
               "no space");

  atomic_nums_[activeMolIdx_].clear();
  for (auto at1 : mol.atoms()) {
    atom_syms_[activeMolIdx_].emplace_back(getAtomSymbolAndOrientation(*at1));
    if (!isComplexQuery(at1)) {
      atomic_nums_[activeMolIdx_].emplace_back(at1->getAtomicNum());
    } else {
      atomic_nums_[activeMolIdx_].push_back(0);
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractAtomNotes(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(annotations_.size()) > activeMolIdx_,
               "no space");

  for (auto atom : mol.atoms()) {
    std::string note;
    if (atom->getPropIfPresent(common_properties::atomNote, note)) {
      if (!note.empty()) {
        AnnotationType annot;
        annot.text_ = note;
        calcAnnotationPosition(mol, atom, annot);
        if (annot.rect_.width_ < 0.0) {
          BOOST_LOG(rdWarningLog) << "Couldn't find good place for note "
                                  << note << " for atom " << atom->getIdx()
                                  << endl;
        } else {
          annotations_[activeMolIdx_].push_back(annot);
        }
      }
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractMolNotes(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(annotations_.size()) > activeMolIdx_,
               "no space");

  std::string note;
  // the molNote property takes priority
  if (!mol.getPropIfPresent(common_properties::molNote, note)) {
    unsigned int chiralFlag;
    if (drawOptions().includeChiralFlagLabel &&
        mol.getPropIfPresent(common_properties::_MolFileChiralFlag,
                             chiralFlag) &&
        chiralFlag) {
      note = "ABS";
    }
  }

  if (!note.empty()) {
    AnnotationType annot;
    annot.text_ = note;
    annot.align_ = TextAlignType::START;
    annot.scaleText_ = false;
    calcAnnotationPosition(mol, annot);
    if (annot.rect_.width_ < 0.0) {
      BOOST_LOG(rdWarningLog) << "Couldn't find good place for molecule note "
                              << note << endl;
    } else {
      annotations_[activeMolIdx_].push_back(annot);
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractBondNotes(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(annotations_.size()) > activeMolIdx_,
               "no space");

  for (auto bond : mol.bonds()) {
    std::string note;
    if (bond->getPropIfPresent(common_properties::bondNote, note)) {
      if (!note.empty()) {
        AnnotationType annot;
        annot.text_ = note;
        calcAnnotationPosition(mol, bond, annot);
        if (annot.rect_.width_ < 0.0) {
          BOOST_LOG(rdWarningLog) << "Couldn't find good place for note "
                                  << note << " for bond " << bond->getIdx()
                                  << endl;
        } else {
          annotations_[activeMolIdx_].push_back(annot);
        }
      }
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractRadicals(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(radicals_.size()) > activeMolIdx_, "no space");

  for (auto atom : mol.atoms()) {
    if (!atom->getNumRadicalElectrons()) {
      continue;
    }
    std::shared_ptr<StringRect> rad_rect(new StringRect);
    OrientType orient = calcRadicalRect(mol, atom, *rad_rect);
    radicals_[activeMolIdx_].push_back(make_pair(rad_rect, orient));
  }
}

// ****************************************************************************
void MolDraw2D::extractLinkNodes(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(post_shapes_.size()) > activeMolIdx_,
               "no space");
  PRECONDITION(static_cast<int>(annotations_.size()) > activeMolIdx_,
               "no space");
  if (!mol.hasProp(common_properties::molFileLinkNodes)) {
    return;
  }

  bool strict = false;
  auto linkNodes = MolEnumerator::utils::getMolLinkNodes(mol, strict);
  for (const auto &node : linkNodes) {
    const double crossingFrac = 0.333;
    const double lengthFrac = 0.333;
    Point2D labelPt{-1000, -1000};
    Point2D labelPerp{0, 0};
    for (const auto &bAts : node.bondAtoms) {
      // unlike brackets, we know how these point
      Point2D startLoc = at_cds_[activeMolIdx_][bAts.first];
      Point2D endLoc = at_cds_[activeMolIdx_][bAts.second];
      auto vect = endLoc - startLoc;
      auto offset = vect * crossingFrac;
      auto crossingPt = startLoc + offset;
      Point2D perp{vect.y, -vect.x};
      perp *= lengthFrac;
      Point2D p1 = crossingPt + perp / 2.;
      Point2D p2 = crossingPt - perp / 2.;

      std::vector<std::pair<Point2D, Point2D>> bondSegments;  // not needed here
      MolDrawShape shp;
      shp.points =
          MolDraw2D_detail::getBracketPoints(p1, p2, startLoc, bondSegments);
      shp.shapeType = MolDrawShapeType::Polyline;
      post_shapes_[activeMolIdx_].emplace_back(std::move(shp));

      if (p1.x > labelPt.x) {
        labelPt = p1;
        labelPerp = crossingPt - startLoc;
      }
      if (p2.x > labelPt.x) {
        labelPt = p2;
        labelPerp = crossingPt - startLoc;
      }
    }

    // the label
    if (supportsAnnotations()) {
      std::string label =
          (boost::format("(%d-%d)") % node.minRep % node.maxRep).str();
      StringRect rect;
      Point2D perp = labelPerp;
      perp /= perp.length() * 5;
      rect.trans_ = labelPt + perp;
      AnnotationType annot;
      annot.text_ = label;
      annot.rect_ = rect;
      annot.align_ = TextAlignType::START;
      annotations_[activeMolIdx_].push_back(annot);
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractBrackets(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(post_shapes_.size()) > activeMolIdx_,
               "no space");
  PRECONDITION(static_cast<int>(annotations_.size()) > activeMolIdx_,
               "no space");
  auto &sgs = getSubstanceGroups(mol);
  if (sgs.empty()) {
    return;
  }
  // details of this transformation are in extractAtomCoords
  double rot = -drawOptions().rotate * M_PI / 180.0;
  RDGeom::Transform2D trans;
  trans.SetTransform(Point2D(0.0, 0.0), rot);
  for (auto &sg : sgs) {
    if (sg.getBrackets().empty()) {
      continue;
    }
    // figure out the location of the reference point we'll use to figure out
    // which direction the bracket points
    // Thanks to John Mayfield for the thoughts on the best way to do this:
    //   http://efficientbits.blogspot.com/2015/11/bringing-molfile-sgroups-to-cdk.html
    Point2D refPt{0., 0.};
    if (!sg.getAtoms().empty()) {
      // use the average position of the atoms in the sgroup
      for (auto aidx : sg.getAtoms()) {
        refPt += at_cds_[activeMolIdx_][aidx];
      }
      refPt /= sg.getAtoms().size();
    }

    std::vector<std::pair<Point2D, Point2D>> sgBondSegments;
    for (auto bndIdx : sg.getBonds()) {
      const auto bnd = mol.getBondWithIdx(bndIdx);
      if (std::find(sg.getAtoms().begin(), sg.getAtoms().end(),
                    bnd->getBeginAtomIdx()) != sg.getAtoms().end()) {
        sgBondSegments.push_back(
            std::make_pair(at_cds_[activeMolIdx_][bnd->getBeginAtomIdx()],
                           at_cds_[activeMolIdx_][bnd->getEndAtomIdx()]));

      } else if (std::find(sg.getAtoms().begin(), sg.getAtoms().end(),
                           bnd->getEndAtomIdx()) != sg.getAtoms().end()) {
        sgBondSegments.push_back(
            std::make_pair(at_cds_[activeMolIdx_][bnd->getEndAtomIdx()],
                           at_cds_[activeMolIdx_][bnd->getBeginAtomIdx()]));
      }
    }
    for (const auto &brk : sg.getBrackets()) {
      Point2D p1{brk[0]};
      Point2D p2{brk[1]};
      trans.TransformPoint(p1);
      trans.TransformPoint(p2);
      MolDrawShape shp;
      shp.points =
          MolDraw2D_detail::getBracketPoints(p1, p2, refPt, sgBondSegments);
      shp.shapeType = MolDrawShapeType::Polyline;
      post_shapes_[activeMolIdx_].emplace_back(std::move(shp));
    }
    if (supportsAnnotations()) {
      // FIX: we could imagine changing this to always show the annotations on
      // the right-most (or bottom-most) bracket

      std::string connect;
      if (sg.getPropIfPresent("CONNECT", connect)) {
        // annotations go on the last bracket of an sgroup
        const auto &brkShp = post_shapes_[activeMolIdx_].back();
        StringRect rect;
        // CONNECT goes at the top
        auto topPt = brkShp.points[1];
        auto brkPt = brkShp.points[0];
        if (brkShp.points[2].y > topPt.y) {
          topPt = brkShp.points[2];
          brkPt = brkShp.points[3];
        }
        rect.trans_ = topPt + (topPt - brkPt);
        AnnotationType annot;
        annot.text_ = connect;
        annot.rect_ = rect;
        // if we're to the right of the bracket, we need to left justify,
        // otherwise things seem to work as is
        if (brkPt.x < topPt.x) {
          annot.align_ = TextAlignType::START;
        }
        annotations_[activeMolIdx_].push_back(annot);
      }
      std::string label;
      if (sg.getPropIfPresent("LABEL", label)) {
        // annotations go on the last bracket of an sgroup
        const auto &brkShp = post_shapes_[activeMolIdx_].back();
        StringRect rect;
        // LABEL goes at the bottom
        auto botPt = brkShp.points[2];
        auto brkPt = brkShp.points[3];
        if (brkShp.points[1].y < botPt.y) {
          botPt = brkShp.points[1];
          brkPt = brkShp.points[0];
        }
        rect.trans_ = botPt + (botPt - brkPt);
        AnnotationType annot;
        annot.text_ = label;
        annot.rect_ = rect;
        annotations_[activeMolIdx_].push_back(annot);
      }
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractSGroupData(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(annotations_.size()) > activeMolIdx_,
               "no space");

  if (!supportsAnnotations()) {
    return;
  }
  auto &sgs = getSubstanceGroups(mol);
  if (sgs.empty()) {
    return;
  }

  // details of this transformation are in extractAtomCoords
  double rot = -drawOptions().rotate * M_PI / 180.0;
  RDGeom::Transform2D tform;
  tform.SetTransform(Point2D(0.0, 0.0), rot);

  for (const auto &sg : sgs) {
    std::string typ;
    if (sg.getPropIfPresent("TYPE", typ) && typ == "DAT") {
      std::string text;
      // it seems like we should be rendering FIELDNAME, but
      // Marvin Sketch, Biovia Draw, and ChemDraw don't do it
      // if (sg.getPropIfPresent("FIELDNAME", text)) {
      //   text += "=";
      // };
      if (sg.hasProp("DATAFIELDS")) {
        STR_VECT dfs = sg.getProp<STR_VECT>("DATAFIELDS");
        for (const auto &df : dfs) {
          text += df + "|";
        }
        text.pop_back();
      }
      if (text.empty()) {
        continue;
      }
      int atomIdx = -1;
      if (!sg.getAtoms().empty()) {
        atomIdx = sg.getAtoms()[0];
      };
      StringRect rect;
      bool located = false;
      std::string fieldDisp;
      if (sg.getPropIfPresent("FIELDDISP", fieldDisp)) {
        double xp = FileParserUtils::stripSpacesAndCast<double>(
            fieldDisp.substr(0, 10));
        double yp = FileParserUtils::stripSpacesAndCast<double>(
            fieldDisp.substr(10, 10));
        Point2D origLoc{xp, yp};

        if (fieldDisp[25] == 'R') {
          if (atomIdx < 0) {
            // we will warn about this below
            text = "";
          } else if (fabs(xp) > 1e-3 || fabs(yp) > 1e-3) {
            origLoc += mol.getConformer().getAtomPos(atomIdx);
            located = true;
          }
        } else {
          if (mol.hasProp("_centroidx")) {
            Point2D centroid;
            mol.getProp("_centroidx", centroid.x);
            mol.getProp("_centroidy", centroid.y);
            origLoc += centroid;
          }
          located = true;
        }
        tform.TransformPoint(origLoc);
        rect.trans_ = origLoc;
      }

      if (!text.empty()) {
        AnnotationType annot;
        annot.text_ = text;
        // looks like everybody renders these left justified
        annot.align_ = TextAlignType::START;
        if (!located) {
          if (atomIdx >= 0 && !text.empty()) {
            calcAnnotationPosition(mol, mol.getAtomWithIdx(atomIdx), annot);
          }
        } else {
          annot.rect_ = rect;
        }
        annotations_[activeMolIdx_].push_back(annot);
      } else {
        BOOST_LOG(rdWarningLog)
            << "FIELDDISP info not found for DAT SGroup which isn't "
               "associated with an atom. SGroup will not be rendered."
            << std::endl;
      }
    }
  }
}

// ****************************************************************************
void MolDraw2D::extractVariableBonds(const ROMol &mol) {
  PRECONDITION(activeMolIdx_ >= 0, "no mol id");
  PRECONDITION(static_cast<int>(pre_shapes_.size()) > activeMolIdx_,
               "no space");
  PRECONDITION(static_cast<int>(annotations_.size()) > activeMolIdx_,
               "no space");

  boost::dynamic_bitset<> atomsInvolved(mol.getNumAtoms());
  for (const auto bond : mol.bonds()) {
    std::string endpts;
    std::string attach;
    if (bond->getPropIfPresent(common_properties::_MolFileBondEndPts, endpts) &&
        bond->getPropIfPresent(common_properties::_MolFileBondAttach, attach)) {
      // FIX: maybe distinguish between "ANY" and "ALL" values of attach here?
      std::vector<unsigned int> oats =
          RDKit::SGroupParsing::ParseV3000Array<unsigned int>(endpts);
      atomsInvolved.reset();
      // decrement the indices and do error checking:
      for (auto &oat : oats) {
        if (oat == 0 || oat > mol.getNumAtoms()) {
          throw ValueErrorException("Bad variation point index");
        }
        --oat;
        atomsInvolved.set(oat);
        MolDrawShape shp;
        shp.shapeType = MolDrawShapeType::Ellipse;
        shp.lineWidth = 1;
        shp.lineColour = drawOptions().variableAttachmentColour;
        shp.fill = true;
        auto center = at_cds_[activeMolIdx_][oat];
        Point2D offset{drawOptions().variableAtomRadius,
                       drawOptions().variableAtomRadius};
        shp.points = {center + offset, center - offset};
        pre_shapes_[activeMolIdx_].emplace_back(std::move(shp));
      }

      for (const auto bond : mol.bonds()) {
        if (atomsInvolved[bond->getBeginAtomIdx()] &&
            atomsInvolved[bond->getEndAtomIdx()]) {
          MolDrawShape shp;
          shp.shapeType = MolDrawShapeType::Polyline;
          shp.lineWidth =
              lineWidth() * drawOptions().variableBondWidthMultiplier;
          shp.scaleLineWidth = true;
          shp.lineColour = drawOptions().variableAttachmentColour;
          shp.fill = false;
          shp.points = {at_cds_[activeMolIdx_][bond->getBeginAtomIdx()],
                        at_cds_[activeMolIdx_][bond->getEndAtomIdx()]};
          pre_shapes_[activeMolIdx_].emplace_back(std::move(shp));
        }
      }
      // correct the symbol of the end atom (remove the *):
      if (!bond->getBeginAtom()->getAtomicNum()) {
        atom_syms_[activeMolIdx_][bond->getBeginAtomIdx()] =
            std::make_pair("", OrientType::C);
      }
    }
  }
}

namespace {
const DashPattern noDash;
const DashPattern dots = assign::list_of(2)(6);
const DashPattern dashes = assign::list_of(6)(6);
const DashPattern shortDashes = assign::list_of(2)(2);

// ****************************************************************************
void drawWedgedBond(MolDraw2D &d2d, const Bond &bond, bool inverted,
                    const Point2D &cds1, const Point2D &cds2, bool draw_dashed,
                    const DrawColour &col1, const DrawColour &col2) {
  if (!d2d.drawOptions().splitBonds) {
    if (inverted) {
      d2d.setActiveAtmIdx(bond.getEndAtomIdx(), bond.getBeginAtomIdx());
    } else {
      d2d.setActiveAtmIdx(bond.getBeginAtomIdx(), bond.getEndAtomIdx());
    }
  }

  Point2D perp = calcPerpendicular(cds1, cds2);
  Point2D disp = perp * 0.15;
  // make sure the displacement isn't too large using the current scale factor
  // (part of github #985)
  // the constants are empirical to make sure that the wedge is visible, but
  // not absurdly large.
  if (d2d.scale() > 40) {
    disp *= .6;
  }
  Point2D end1 = cds2 + disp;
  Point2D end2 = cds2 - disp;

  d2d.setColour(col1);
  if (draw_dashed) {
    d2d.setFillPolys(false);

    unsigned int nDashes;
    // empirical cutoff to make sure we don't have too many dashes in the
    // wedge:
    auto factor = d2d.scale() * (cds1 - cds2).lengthSq();
    if (factor < 20) {
      nDashes = 3;
    } else if (factor < 30) {
      nDashes = 4;
    } else if (factor < 45) {
      nDashes = 5;
    } else {
      nDashes = 6;
    }

    int orig_lw = d2d.lineWidth();
    int tgt_lw = 1;  // use the minimum line width
    d2d.setLineWidth(tgt_lw);

    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(inverted ? bond.getEndAtomIdx()
                                   : bond.getBeginAtomIdx());
    }
    Point2D e1 = end1 - cds1;
    Point2D e2 = end2 - cds1;
    for (unsigned int i = 1; i < nDashes + 1; ++i) {
      if ((nDashes / 2 + 1) == i) {
        d2d.setColour(col2);
        if (d2d.drawOptions().splitBonds) {
          d2d.setActiveAtmIdx(inverted ? bond.getBeginAtomIdx()
                                       : bond.getEndAtomIdx());
        }
      }
      Point2D e11 = cds1 + e1 * (rdcast<double>(i) / nDashes);
      Point2D e22 = cds1 + e2 * (rdcast<double>(i) / nDashes);
      if (d2d.drawOptions().comicMode) {
        auto pts = MolDraw2D_detail::handdrawnLine(e11, e22, d2d.scale());
        d2d.drawPolygon(pts);
      } else {
        d2d.drawLine(e11, e22);
      }
    }
    d2d.setLineWidth(orig_lw);
  } else {
    d2d.setFillPolys(true);
    if (col1 == col2 && !d2d.drawOptions().splitBonds) {
      d2d.drawTriangle(cds1, end1, end2);
    } else {
      if (d2d.drawOptions().splitBonds) {
        d2d.setActiveAtmIdx(inverted ? bond.getEndAtomIdx()
                                     : bond.getBeginAtomIdx());
      }
      Point2D e1 = end1 - cds1;
      Point2D e2 = end2 - cds1;
      Point2D mid1 = cds1 + e1 * 0.5;
      Point2D mid2 = cds1 + e2 * 0.5;
      d2d.drawTriangle(cds1, mid1, mid2);
      if (d2d.drawOptions().splitBonds) {
        d2d.setActiveAtmIdx(inverted ? bond.getBeginAtomIdx()
                                     : bond.getEndAtomIdx());
      }
      d2d.setColour(col2);
      d2d.drawTriangle(mid1, end2, end1);
      d2d.drawTriangle(mid1, mid2, end2);
    }
  }
  d2d.setActiveAtmIdx();
}

// ****************************************************************************
void drawDativeBond(MolDraw2D &d2d, const Bond &bond, const Point2D &cds1,
                    const Point2D &cds2, const DrawColour &col1,
                    const DrawColour &col2) {
  if (!d2d.drawOptions().splitBonds) {
    d2d.setActiveAtmIdx(bond.getBeginAtomIdx(), bond.getEndAtomIdx());
  } else {
    d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
  }

  Point2D mid = (cds1 + cds2) * 0.5;
  d2d.drawLine(cds1, mid, col1, col1);

  if (d2d.drawOptions().splitBonds) {
    d2d.setActiveAtmIdx(bond.getEndAtomIdx());
  }
  d2d.setColour(col2);
  bool asPolygon = true;
  double frac = 0.2;
  double angle = M_PI / 6;
  // the polygon triangle at the end extends past cds2, so step back a bit
  // so as not to trample on anything else.
  Point2D delta = mid - cds2;
  Point2D end = cds2 + delta * frac;
  d2d.drawArrow(mid, end, asPolygon, frac, angle);
  d2d.setActiveAtmIdx();
}

void drawBondLine(MolDraw2D &d2d, const Bond &bond, const Point2D &cds1,
                  const Point2D &cds2, const DrawColour &col1,
                  const DrawColour &col2, bool clearAIdx = true) {
  if (!d2d.drawOptions().splitBonds) {
    d2d.setActiveAtmIdx(bond.getBeginAtomIdx(), bond.getEndAtomIdx());
    d2d.drawLine(cds1, cds2, col1, col2);
    if (clearAIdx) {
      d2d.setActiveAtmIdx();
    }
    return;
  }
  Point2D mid = (cds1 + cds2) * 0.5;
  d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
  d2d.drawLine(cds1, mid, col1, col1);
  d2d.setActiveAtmIdx(bond.getEndAtomIdx());
  d2d.drawLine(mid, cds2, col2, col2);
  if (clearAIdx) {
    d2d.setActiveAtmIdx();
  }
}

void drawBondLine(MolDraw2D &d2d, const Bond &bond, const Point2D &cds1,
                  const Point2D &cds2, bool clearAIdx = true) {
  if (!d2d.drawOptions().splitBonds) {
    d2d.drawLine(cds1, cds2);
    if (clearAIdx) {
      d2d.setActiveAtmIdx();
    }
    return;
  }
  const auto midp = (cds1 + cds2) / 2;
  d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
  d2d.drawLine(cds1, midp);
  d2d.setActiveAtmIdx(bond.getEndAtomIdx());
  d2d.drawLine(midp, cds2);
  if (clearAIdx) {
    d2d.setActiveAtmIdx();
  }
}

void drawBondWavyLine(MolDraw2D &d2d, const Bond &bond, const Point2D &cds1,
                      const Point2D &cds2, const DrawColour &col1,
                      const DrawColour &col2) {
  // as splitting wavy line might cause rendering problems
  // do not split and flag wavy bond with both atoms
  d2d.setActiveAtmIdx(bond.getBeginAtomIdx(), bond.getEndAtomIdx());
  d2d.drawWavyLine(cds1, cds2, col1, col2);
  d2d.setActiveAtmIdx();
}

void drawNormalBond(MolDraw2D &d2d, const Bond &bond, bool highlight_bond,
                    Point2D at1_cds, Point2D at2_cds,
                    const std::vector<Point2D> &at_cds, DrawColour col1,
                    DrawColour col2, double double_bond_offset) {
  auto bt = bond.getBondType();
  auto &mol = bond.getOwningMol();
  // it's a double bond and one end is 1-connected, do two lines parallel
  // to the atom-atom line.
  if (bt == Bond::DOUBLE || bt == Bond::AROMATIC) {
    Point2D l1s, l1f, l2s, l2f;
    calcDoubleBondLines(mol, double_bond_offset, bond, at1_cds, at2_cds, at_cds,
                        l1s, l1f, l2s, l2f);
    bool orig_slw = d2d.drawOptions().scaleBondWidth;
    if (highlight_bond) {
      d2d.drawOptions().scaleBondWidth =
          d2d.drawOptions().scaleHighlightBondWidth;
    }
    drawBondLine(d2d, bond, l1s, l1f, col1, col2);
    if (bt == Bond::AROMATIC) {
      d2d.setDash(dashes);
    }
    drawBondLine(d2d, bond, l2s, l2f, col1, col2);
    if (bt == Bond::AROMATIC) {
      d2d.setDash(noDash);
    }
    d2d.drawOptions().scaleBondWidth = orig_slw;
  } else if (Bond::SINGLE == bt && (Bond::BEGINWEDGE == bond.getBondDir() ||
                                    Bond::BEGINDASH == bond.getBondDir())) {
    // swap the direction if at1 has does not have stereochem set
    // or if at2 does have stereochem set and the bond starts there
    auto at1 = bond.getBeginAtom();
    auto at2 = bond.getEndAtom();
    auto inverted = false;
    if ((at1->getChiralTag() != Atom::CHI_TETRAHEDRAL_CW &&
         at1->getChiralTag() != Atom::CHI_TETRAHEDRAL_CCW) ||
        (at1->getIdx() != bond.getBeginAtomIdx() &&
         (at2->getChiralTag() == Atom::CHI_TETRAHEDRAL_CW ||
          at2->getChiralTag() == Atom::CHI_TETRAHEDRAL_CCW))) {
      // std::cerr << "  swap" << std::endl;
      swap(at1_cds, at2_cds);
      swap(col1, col2);
      inverted = true;
    }
    if (d2d.drawOptions().singleColourWedgeBonds) {
      col1 = d2d.drawOptions().symbolColour;
      col2 = d2d.drawOptions().symbolColour;
    }
    // deliberately not scaling highlighted bond width
    if (Bond::BEGINWEDGE == bond.getBondDir()) {
      drawWedgedBond(d2d, bond, inverted, at1_cds, at2_cds, false, col1, col2);
    } else {
      drawWedgedBond(d2d, bond, inverted, at1_cds, at2_cds, true, col1, col2);
    }
  } else if (Bond::SINGLE == bt && Bond::UNKNOWN == bond.getBondDir()) {
    // unspecified stereo
    // deliberately not scaling highlighted bond width
    drawBondWavyLine(d2d, bond, at1_cds, at2_cds, col1, col2);
  } else if (Bond::DATIVE == bt || Bond::DATIVEL == bt || Bond::DATIVER == bt) {
    // deliberately not scaling highlighted bond width as I think
    // the arrowhead will look ugly.
    drawDativeBond(d2d, bond, at1_cds, at2_cds, col1, col2);
  } else if (Bond::ZERO == bt) {
    d2d.setDash(shortDashes);
    bool orig_slw = d2d.drawOptions().scaleBondWidth;
    if (highlight_bond) {
      d2d.drawOptions().scaleBondWidth =
          d2d.drawOptions().scaleHighlightBondWidth;
    }
    drawBondLine(d2d, bond, at1_cds, at2_cds, col1, col2);
    d2d.drawOptions().scaleBondWidth = orig_slw;
    d2d.setDash(noDash);
  } else if (Bond::HYDROGEN == bt) {
    d2d.setDash(dots);
    bool orig_slw = d2d.drawOptions().scaleBondWidth;
    if (highlight_bond) {
      d2d.drawOptions().scaleBondWidth =
          d2d.drawOptions().scaleHighlightBondWidth;
    }
    drawBondLine(d2d, bond, at1_cds, at2_cds, DrawColour(0.2, 0.2, 0.2),
                 DrawColour(0.2, 0.2, 0.2));
    d2d.drawOptions().scaleBondWidth = orig_slw;
    d2d.setDash(noDash);
  } else {
    // in all other cases, we will definitely want to draw a line between
    // the two atoms
    bool orig_slw = d2d.drawOptions().scaleBondWidth;
    if (highlight_bond) {
      d2d.drawOptions().scaleBondWidth =
          d2d.drawOptions().scaleHighlightBondWidth;
    }
    drawBondLine(d2d, bond, at1_cds, at2_cds, col1, col2);
    if (Bond::TRIPLE == bt) {
      Point2D l1s, l1f, l2s, l2f;
      calcTripleBondLines(double_bond_offset, bond, at1_cds, at2_cds, l1s, l1f,
                          l2s, l2f);
      drawBondLine(d2d, bond, l1s, l1f, col1, col2);
      drawBondLine(d2d, bond, l2s, l2f, col1, col2);
    }
    d2d.drawOptions().scaleBondWidth = orig_slw;
  }
}

void drawQueryBond1(MolDraw2D &d2d, const Bond &bond, bool highlight_bond,
                    const Point2D &at1_cds, const Point2D &at2_cds,
                    const std::vector<Point2D> &at_cds, const DrawColour &col1,
                    const DrawColour &col2, double double_bond_offset) {
  PRECONDITION(bond.hasQuery(), "no query");
  const auto qry = bond.getQuery();
  if (!d2d.drawOptions().splitBonds) {
    d2d.setActiveAtmIdx(bond.getBeginAtomIdx(), bond.getEndAtomIdx());
  }
  auto midp = (at2_cds + at1_cds) / 2.;
  auto dv = at2_cds - at1_cds;
  auto p1 = at1_cds + dv * (1. / 3.);
  auto p2 = at1_cds + dv * (2. / 3.);
  auto tdash = shortDashes;
  if (d2d.scale() < 10) {
    tdash[0] /= 4;
    tdash[1] /= 3;
  } else if (d2d.scale() < 20) {
    tdash[0] /= 2;
    tdash[1] /= 1.5;
  }
  if (qry->getDescription() == "SingleOrDoubleBond") {
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond,
                          at1_cds, p1, at_cds, l1s, l1f, l2s, l2f);
      d2d.setColour(col1);
      d2d.drawLine(l1s, l1f);
      d2d.drawLine(l2s, l2f);
    }
    drawBondLine(d2d, bond, p1, p2, col1, col2, false);
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, p2,
                          at2_cds, at_cds, l1s, l1f, l2s, l2f);
      d2d.setColour(col2);
      d2d.drawLine(l1s, l1f);
      d2d.drawLine(l2s, l2f);
    }
  } else if (qry->getDescription() == "SingleOrAromaticBond") {
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond,
                          at1_cds, p1, at_cds, l1s, l1f, l2s, l2f);
      d2d.setColour(col1);
      d2d.drawLine(l1s, l1f);
      d2d.setDash(tdash);
      d2d.drawLine(l2s, l2f);
      d2d.setDash(noDash);
    }
    drawBondLine(d2d, bond, p1, p2, col1, col2, false);
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, p2,
                          at2_cds, at_cds, l1s, l1f, l2s, l2f);
      d2d.setColour(col2);
      d2d.drawLine(l1s, l1f);
      d2d.setDash(tdash);
      d2d.drawLine(l2s, l2f);
      d2d.setDash(noDash);
    }
  } else if (qry->getDescription() == "DoubleOrAromaticBond") {
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond,
                          at1_cds, p1, at_cds, l1s, l1f, l2s, l2f);
      d2d.setColour(col1);
      d2d.drawLine(l1s, l1f);
      d2d.setDash(tdash);
      d2d.drawLine(l2s, l2f);
      d2d.setDash(noDash);
    }
    if (d2d.drawOptions().splitBonds) {
      {
        Point2D l1s, l1f, l2s, l2f;
        calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, p1,
                            midp, at_cds, l1s, l1f, l2s, l2f);
        d2d.setColour(col1);
        d2d.drawLine(l1s, l1f, col1, col2);
        d2d.drawLine(l2s, l2f, col1, col2);
        d2d.setDash(noDash);
      }
      d2d.setActiveAtmIdx(bond.getEndAtomIdx());
      {
        Point2D l1s, l1f, l2s, l2f;
        calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, midp,
                            p2, at_cds, l1s, l1f, l2s, l2f);
        d2d.setColour(col1);
        d2d.drawLine(l1s, l1f, col1, col2);
        d2d.drawLine(l2s, l2f, col1, col2);
        d2d.setDash(noDash);
      }
    } else {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, p1, p2,
                          at_cds, l1s, l1f, l2s, l2f);
      d2d.setColour(col1);
      d2d.drawLine(l1s, l1f, col1, col2);
      d2d.drawLine(l2s, l2f, col1, col2);
      d2d.setDash(noDash);
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, p2,
                          at2_cds, at_cds, l1s, l1f, l2s, l2f);
      d2d.setColour(col2);
      d2d.drawLine(l1s, l1f);
      d2d.setDash(tdash);
      d2d.drawLine(l2s, l2f);
      d2d.setDash(noDash);
    }
  } else if (qry->getDescription() == "BondNull") {
    d2d.setDash(tdash);
    bool orig_slw = d2d.drawOptions().scaleBondWidth;
    if (highlight_bond) {
      d2d.drawOptions().scaleBondWidth =
          d2d.drawOptions().scaleHighlightBondWidth;
    }
    drawBondLine(d2d, bond, at1_cds, at2_cds, col1, col2, false);
    d2d.drawOptions().scaleBondWidth = orig_slw;
    d2d.setDash(noDash);
  } else {
    d2d.setDash(dots);
    bool orig_slw = d2d.drawOptions().scaleBondWidth;
    if (highlight_bond) {
      d2d.drawOptions().scaleBondWidth =
          d2d.drawOptions().scaleHighlightBondWidth;
    }
    drawBondLine(d2d, bond, at1_cds, at2_cds, col1, col2, false);
    d2d.drawOptions().scaleBondWidth = orig_slw;
    d2d.setDash(noDash);
  }
  d2d.setActiveAtmIdx();
}

void drawQueryBond(MolDraw2D &d2d, const Bond &bond, bool highlight_bond,
                   const Point2D &at1_cds, const Point2D &at2_cds,
                   const std::vector<Point2D> &at_cds,
                   double double_bond_offset) {
  PRECONDITION(bond.hasQuery(), "no query");
  const auto qry = bond.getQuery();
  if (!d2d.drawOptions().splitBonds) {
    d2d.setActiveAtmIdx(bond.getBeginAtomIdx(), bond.getEndAtomIdx());
  }
  auto midp = (at2_cds + at1_cds) / 2.;
  auto tdash = shortDashes;
  if (d2d.scale() < 10) {
    tdash[0] /= 4;
    tdash[1] /= 3;
  } else if (d2d.scale() < 20) {
    tdash[0] /= 2;
    tdash[1] /= 1.5;
  }
  DrawColour queryColour{0.5, 0.5, 0.5};
  d2d.setColour(queryColour);

  bool drawGenericQuery = false;
  if (qry->getDescription() == "SingleOrDoubleBond") {
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
    }
    d2d.drawLine(at1_cds, midp);
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getEndAtomIdx());
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, midp,
                          at2_cds, at_cds, l1s, l1f, l2s, l2f);
      d2d.drawLine(l1s, l1f);
      d2d.drawLine(l2s, l2f);
    }
  } else if (qry->getDescription() == "SingleOrAromaticBond") {
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
    }
    d2d.drawLine(at1_cds, midp);
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getEndAtomIdx());
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, midp,
                          at2_cds, at_cds, l1s, l1f, l2s, l2f);
      d2d.drawLine(l1s, l1f);
      d2d.setDash(tdash);
      d2d.drawLine(l2s, l2f);
      d2d.setDash(noDash);
    }
  } else if (qry->getDescription() == "DoubleOrAromaticBond") {
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getBeginAtomIdx());
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond,
                          at1_cds, midp, at_cds, l1s, l1f, l2s, l2f);
      d2d.drawLine(l1s, l1f);
      d2d.drawLine(l2s, l2f);
    }
    if (d2d.drawOptions().splitBonds) {
      d2d.setActiveAtmIdx(bond.getEndAtomIdx());
    }
    {
      Point2D l1s, l1f, l2s, l2f;
      calcDoubleBondLines(bond.getOwningMol(), double_bond_offset, bond, midp,
                          at2_cds, at_cds, l1s, l1f, l2s, l2f);
      d2d.drawLine(l1s, l1f);
      d2d.setDash(tdash);
      d2d.drawLine(l2s, l2f);
      d2d.setDash(noDash);
    }
  } else if (qry->getDescription() == "BondNull") {
    d2d.setDash(tdash);
    drawBondLine(d2d, bond, at1_cds, at2_cds);
    d2d.setDash(noDash);
  } else if (qry->getDescription() == "BondAnd" &&
             qry->endChildren() - qry->beginChildren() == 2) {
    auto q1 = *(qry->beginChildren());
    auto q2 = *(qry->beginChildren() + 1);

    if (q2->getDescription() == "BondOrder") {
      std::swap(q1, q2);
    }
    if (q1->getDescription() == "BondOrder" &&
        q2->getDescription() == "BondInRing") {
      drawNormalBond(d2d, bond, false, at1_cds, at2_cds, at_cds, queryColour,
                     queryColour, double_bond_offset);

      Point2D segment = at2_cds - at1_cds;
      d2d.setFillPolys(false);
      auto slw = d2d.drawOptions().scaleBondWidth;
      d2d.drawOptions().scaleBondWidth = false;
      auto lw = d2d.lineWidth();
      d2d.setLineWidth(1);
      if (!q2->getNegation()) {
        segment /= segment.length() * 6;
        Point2D r1 = Point2D(0.5 * segment.x - 0.866 * segment.y,
                             0.866 * segment.x + 0.5 * segment.y);
        Point2D r2 =
            Point2D(0.5 * r1.x - 0.866 * r1.y, 0.866 * r1.x + 0.5 * r1.y);
        std::vector<Point2D> pts = {midp + segment, midp + r1, midp + r2,
                                    midp - segment, midp - r1, midp - r2,
                                    midp + segment};
        d2d.drawPolygon(pts);

      } else {
        segment /= segment.length() * 10;
        auto l = segment.length();
        Point2D p1 = midp + segment + Point2D(l, l);
        Point2D p2 = midp + segment - Point2D(l, l);
        d2d.drawEllipse(p1, p2);
        p1 = midp - segment + Point2D(l, l);
        p2 = midp - segment - Point2D(l, l);
        d2d.drawEllipse(p1, p2);
      }
      d2d.drawOptions().scaleBondWidth = slw;
      d2d.setLineWidth(lw);
    } else {
      drawGenericQuery = true;
    }
  } else {
    drawGenericQuery = true;
  }
  if (drawGenericQuery) {
    d2d.setDash(dots);
    bool orig_slw = d2d.drawOptions().scaleBondWidth;
    if (highlight_bond) {
      d2d.drawOptions().scaleBondWidth =
          d2d.drawOptions().scaleHighlightBondWidth;
    }
    drawBondLine(d2d, bond, at1_cds, at2_cds);
    d2d.drawOptions().scaleBondWidth = orig_slw;
    d2d.setDash(noDash);
  }
  d2d.setActiveAtmIdx();
}

}  // namespace

// ****************************************************************************
void MolDraw2D::drawBond(
    const ROMol &, const Bond *bond, int at1_idx, int at2_idx,
    const vector<int> *, const map<int, DrawColour> *,
    const vector<int> *highlight_bonds,
    const map<int, DrawColour> *highlight_bond_map,
    const std::vector<std::pair<DrawColour, DrawColour>> *bond_colours) {
  PRECONDITION(bond, "no bond");
  PRECONDITION(activeMolIdx_ >= 0, "bad mol idx");

  if (static_cast<unsigned int>(at1_idx) != bond->getBeginAtomIdx()) {
    std::swap(at1_idx, at2_idx);
  }

  Point2D at1_cds = at_cds_[activeMolIdx_][at1_idx];
  Point2D at2_cds = at_cds_[activeMolIdx_][at2_idx];

  double double_bond_offset = options_.multipleBondOffset;
  // mol files from, for example, Marvin use a bond length of 1 for just about
  // everything. When this is the case, the default multipleBondOffset is just
  // too much, so scale it back.
  if ((at1_cds - at2_cds).lengthSq() < 1.4) {
    double_bond_offset *= 0.6;
  }

  adjustBondEndForLabel(atom_syms_[activeMolIdx_][at1_idx], at2_cds, at1_cds);
  adjustBondEndForLabel(atom_syms_[activeMolIdx_][at2_idx], at1_cds, at2_cds);

  bool highlight_bond = false;
  if (highlight_bonds &&
      std::find(highlight_bonds->begin(), highlight_bonds->end(),
                bond->getIdx()) != highlight_bonds->end()) {
    highlight_bond = true;
  }

  DrawColour col1, col2;
  int orig_lw = lineWidth();
  if (bond_colours) {
    col1 = (*bond_colours)[bond->getIdx()].first;
    col2 = (*bond_colours)[bond->getIdx()].second;
  } else {
    if (!highlight_bond) {
      col1 = getColour(at1_idx);
      col2 = getColour(at2_idx);
    } else {
      if (highlight_bond_map && highlight_bond_map->find(bond->getIdx()) !=
                                    highlight_bond_map->end()) {
        col1 = col2 = highlight_bond_map->find(bond->getIdx())->second;
      } else {
        col1 = col2 = drawOptions().highlightColour;
      }
      if (drawOptions().continuousHighlight) {
        setLineWidth(getHighlightBondWidth(bond->getIdx(), nullptr));
      } else {
        setLineWidth(getHighlightBondWidth(bond->getIdx(), nullptr) / 4);
      }
    }
  }

  bool isComplex = false;
  if (bond->hasQuery()) {
    std::string descr = bond->getQuery()->getDescription();
    if (bond->getQuery()->getNegation() || descr != "BondOrder") {
      isComplex = true;
      drawQueryBond(*this, *bond, highlight_bond, at1_cds, at2_cds,
                    at_cds_[activeMolIdx_], double_bond_offset);
    }
  }

  if (!isComplex) {
    drawNormalBond(*this, *bond, highlight_bond, at1_cds, at2_cds,
                   at_cds_[activeMolIdx_], col1, col2, double_bond_offset);
  }
  if (highlight_bond) {
    setLineWidth(orig_lw);
  }
}

// ****************************************************************************
void MolDraw2D::drawAtomLabel(int atom_num,
                              const std::vector<int> *highlight_atoms,
                              const std::map<int, DrawColour> *highlight_map) {
  drawAtomLabel(atom_num, getColour(atom_num, highlight_atoms, highlight_map));
}

// ****************************************************************************
void MolDraw2D::drawAtomLabel(int atom_num, const DrawColour &draw_colour) {
  text_drawer_->setColour(draw_colour);
  Point2D draw_cds = getDrawCoords(atom_num);
  text_drawer_->drawString(atom_syms_[activeMolIdx_][atom_num].first, draw_cds,
                           atom_syms_[activeMolIdx_][atom_num].second);
  // this is useful for debugging the drawings.
  //  int olw = lineWidth();
  //  setLineWidth(1);
  //  text_drawer_->drawStringRects(atom_syms_[activeMolIdx_][atom_num].first,
  //                                atom_syms_[activeMolIdx_][atom_num].second,
  //                                draw_cds, *this);
  //  setLineWidth(olw);
}

// ****************************************************************************
void MolDraw2D::drawAnnotation(const AnnotationType &annot) {
  double full_font_scale = text_drawer_->fontScale();
  // turn off minFontSize for the annotation, as we do want it to be smaller
  // than the letters, even if that makes it tiny.  The annotation positions
  // have been calculated on the assumption that this is the case, and if
  // minFontSize is applied, they may well clash with the atom symbols.
  if (annot.scaleText_) {
    text_drawer_->setFontScale(drawOptions().annotationFontScale *
                               full_font_scale, true);
  }
  Point2D draw_cds = getDrawCoords(annot.rect_.trans_);
  text_drawer_->drawString(annot.text_, draw_cds, annot.align_);
  if (annot.scaleText_) {
    text_drawer_->setFontScale(full_font_scale, true);
  }
}

// ****************************************************************************
OrientType MolDraw2D::calcRadicalRect(const ROMol &mol, const Atom *atom,
                                      StringRect &rad_rect) {
  int num_rade = atom->getNumRadicalElectrons();
  double spot_rad = 0.2 * drawOptions().multipleBondOffset;
  Point2D const &at_cds = at_cds_[activeMolIdx_][atom->getIdx()];
  string const &at_sym = atom_syms_[activeMolIdx_][atom->getIdx()].first;
  OrientType orient = atom_syms_[activeMolIdx_][atom->getIdx()].second;
  double rad_size = (4 * num_rade - 2) * spot_rad;
  double x_min, y_min, x_max, y_max;
  Point2D at_draw_cds = getDrawCoords(at_cds);
  if (!at_sym.empty()) {
    text_drawer_->getStringExtremes(at_sym, orient, x_min, y_min, x_max, y_max);
    x_min += at_draw_cds.x;
    x_max += at_draw_cds.x;
    y_min += at_draw_cds.y;
    y_max += at_draw_cds.y;
  } else {
    x_min = at_draw_cds.x - 3 * spot_rad * text_drawer_->fontScale();
    x_max = at_draw_cds.x + 3 * spot_rad * text_drawer_->fontScale();
    y_min = at_draw_cds.y - 3 * spot_rad * text_drawer_->fontScale();
    y_max = at_draw_cds.y + 3 * spot_rad * text_drawer_->fontScale();
  }

  auto rect_to_atom_coords = [&](StringRect &rect) {
    rect.width_ /= text_drawer_->fontScale();
    rect.height_ /= text_drawer_->fontScale();
    rect.trans_ = getAtomCoords(make_pair(rect.trans_.x, rect.trans_.y));
  };

  auto try_all = [&](OrientType ornt) -> bool {
    vector<std::shared_ptr<StringRect>> rad_rects(
        1, std::shared_ptr<StringRect>(new StringRect(rad_rect)));
    if (!text_drawer_->doesRectIntersect(at_sym, ornt, at_cds, rad_rect) &&
        !doesAtomNoteClash(rad_rect.trans_, rad_rects, mol, atom->getIdx())) {
      rect_to_atom_coords(rad_rect);
      return true;
    } else {
      return false;
    }
  };

  auto try_north = [&]() -> bool {
    rad_rect.width_ = rad_size * text_drawer_->fontScale();
    rad_rect.height_ = spot_rad * 3.0 * text_drawer_->fontScale();
    rad_rect.trans_.x = at_draw_cds.x;
    rad_rect.trans_.y = y_max + 0.5 * rad_rect.height_;
    return try_all(OrientType::N);
  };
  auto try_south = [&]() -> bool {
    rad_rect.width_ = rad_size * text_drawer_->fontScale();
    rad_rect.height_ = spot_rad * 3.0 * text_drawer_->fontScale();
    rad_rect.trans_.x = at_draw_cds.x;
    rad_rect.trans_.y = y_min - 0.5 * rad_rect.height_;
    return try_all(OrientType::S);
  };
  auto try_east = [&]() -> bool {
    rad_rect.trans_.x = x_max + 3.0 * spot_rad * text_drawer_->fontScale();
    rad_rect.trans_.y = at_draw_cds.y;
    rad_rect.width_ = spot_rad * 1.5 * text_drawer_->fontScale();
    rad_rect.height_ = rad_size * text_drawer_->fontScale();
    return try_all(OrientType::E);
  };
  auto try_west = [&]() -> bool {
    rad_rect.trans_.x = x_min - 3.0 * spot_rad * text_drawer_->fontScale();
    rad_rect.trans_.y = at_draw_cds.y;
    rad_rect.width_ = spot_rad * 1.5 * text_drawer_->fontScale();
    rad_rect.height_ = rad_size * text_drawer_->fontScale();
    return try_all(OrientType::W);
  };

  auto try_rads = [&](OrientType ornt) -> bool {
    switch (ornt) {
      case OrientType::N:
      case OrientType::C:
        return try_north();
      case OrientType::E:
        return try_east();
      case OrientType::S:
        return try_south();
      case OrientType::W:
        return try_west();
    }
    return false;
  };
  if (try_rads(orient)) {
    return orient;
  }
  OrientType all_ors[4] = {OrientType::N, OrientType::E, OrientType::S,
                           OrientType::W};
  for (int io = 0; io < 4; ++io) {
    if (orient != all_ors[io]) {
      if (try_rads(all_ors[io])) {
        return all_ors[io];
      }
    }
  }
  // stick them N irrespective of a clash whilst muttering "sod it"
  // under our breath.
  try_north();
  return OrientType::N;
}

namespace {}  // namespace

// ****************************************************************************
void MolDraw2D::drawRadicals(const ROMol &mol) {
  // take account of differing font scale and main scale if we've hit
  // max or min font size.
  double f_scale = text_drawer_->fontScale() / scale();
  double spot_rad = 0.2 * drawOptions().multipleBondOffset * f_scale;
  setColour(DrawColour(0.0, 0.0, 0.0));
  // Point2D should be in atom coords
  auto draw_spot = [&](const Point2D &cds) {
    bool ofp = fillPolys();
    setFillPolys(true);
    int olw = lineWidth();
    setLineWidth(0);
    drawArc(cds, spot_rad, 0, 360);
    setLineWidth(olw);
    setFillPolys(ofp);
  };
  // cds in draw coords

  auto draw_spots = [&](const Point2D &cds, int num_spots, double width,
                        int dir = 0) {
    Point2D ncds = cds;
    switch (num_spots) {
      case 3:
        draw_spot(ncds);
        if (dir) {
          ncds.y = cds.y - 0.5 * width + spot_rad;
        } else {
          ncds.x = cds.x - 0.5 * width + spot_rad;
        }
        draw_spot(ncds);
        if (dir) {
          ncds.y = cds.y + 0.5 * width - spot_rad;
        } else {
          ncds.x = cds.x + 0.5 * width - spot_rad;
        }
        draw_spot(ncds);
        /* fallthrough */
      case 1:
        draw_spot(cds);
        break;
      case 4:
        if (dir) {
          ncds.y = cds.y + 6.0 * spot_rad;
        } else {
          ncds.x = cds.x + 6.0 * spot_rad;
        }
        draw_spot(ncds);
        if (dir) {
          ncds.y = cds.y - 6.0 * spot_rad;
        } else {
          ncds.x = cds.x - 6.0 * spot_rad;
        }
        draw_spot(ncds);
        /* fallthrough */
      case 2:
        if (dir) {
          ncds.y = cds.y + 2.0 * spot_rad;
        } else {
          ncds.x = cds.x + 2.0 * spot_rad;
        }
        draw_spot(ncds);
        if (dir) {
          ncds.y = cds.y - 2.0 * spot_rad;
        } else {
          ncds.x = cds.x - 2.0 * spot_rad;
        }
        draw_spot(ncds);
        break;
    }
  };

  size_t rad_num = 0;
  for (auto atom : mol.atoms()) {
    int num_rade = atom->getNumRadicalElectrons();
    if (!num_rade) {
      continue;
    }
    auto rad_rect = radicals_[activeMolIdx_][rad_num].first;
    OrientType draw_or = radicals_[activeMolIdx_][rad_num].second;
    if (draw_or == OrientType::N || draw_or == OrientType::S ||
        draw_or == OrientType::C) {
      draw_spots(rad_rect->trans_, num_rade, rad_rect->width_, 0);
    } else {
      draw_spots(rad_rect->trans_, num_rade, rad_rect->height_, 1);
    }
    ++rad_num;
  }
}

// ****************************************************************************
double MolDraw2D::getNoteStartAngle(const ROMol &mol, const Atom *atom) const {
  if (atom->getDegree() == 0) {
    return M_PI / 2.0;
  }
  Point2D at_cds = at_cds_[activeMolIdx_][atom->getIdx()];
  vector<Point2D> bond_vecs;
  for (const auto &nbr : make_iterator_range(mol.getAtomNeighbors(atom))) {
    Point2D bond_vec = at_cds.directionVector(at_cds_[activeMolIdx_][nbr]);
    bond_vec.normalize();
    bond_vecs.emplace_back(bond_vec);
  }

  Point2D ret_vec;
  if (bond_vecs.size() == 1) {
    if (atom_syms_[activeMolIdx_][atom->getIdx()].first.empty()) {
      // go with perpendicular to bond.  This is mostly to avoid getting
      // a zero at the end of a bond to carbon, which looks like a black
      // oxygen atom in the default font in SVG and PNG.
      ret_vec.x = bond_vecs[0].y;
      ret_vec.y = -bond_vecs[0].x;
    } else {
      // go opposite end
      ret_vec = -bond_vecs[0];
    }
  } else if (bond_vecs.size() == 2) {
    ret_vec = bond_vecs[0] + bond_vecs[1];
    if (ret_vec.lengthSq() > 1.0e-6) {
      if (!atom->getNumImplicitHs() || atom->getAtomicNum() == 6) {
        // prefer outside the angle, unless there are Hs that will be in
        // the way, probably.
        ret_vec *= -1.0;
      }
    } else {
      // it must be a -# or == or some such.  Take perpendicular to
      // one of them
      ret_vec.x = -bond_vecs.front().y;
      ret_vec.y = bond_vecs.front().x;
      ret_vec.normalize();
    }
  } else {
    // just take 2 that are probably adjacent
    double discrim = 4.0 * M_PI / bond_vecs.size();
    for (size_t i = 0; i < bond_vecs.size() - 1; ++i) {
      for (size_t j = i + 1; j < bond_vecs.size(); ++j) {
        double ang = acos(bond_vecs[i].dotProduct(bond_vecs[j]));
        if (ang < discrim) {
          ret_vec = bond_vecs[i] + bond_vecs[j];
          ret_vec.normalize();
          discrim = -1.0;
          break;
        }
      }
    }
    if (discrim > 0.0) {
      ret_vec = bond_vecs[0] + bond_vecs[1];
      ret_vec *= -1.0;
    }
  }

  // start angle is the angle between ret_vec and the x axis
  return atan2(ret_vec.y, ret_vec.x);
}

// ****************************************************************************
int MolDraw2D::doesAtomNoteClash(
    const Point2D &note_pos, const vector<std::shared_ptr<StringRect>> &rects,
    const ROMol &mol, unsigned int atom_idx) const {
  auto atom = mol.getAtomWithIdx(atom_idx);

  if (doesNoteClashNbourBonds(note_pos, rects, mol, atom)) {
    return 1;
  }
  if (doesNoteClashAtomLabels(note_pos, rects, mol, atom_idx)) {
    return 2;
  }
  if (doesNoteClashOtherNotes(note_pos, rects)) {
    return 3;
  }
  return 0;
}

// ****************************************************************************
int MolDraw2D::doesBondNoteClash(
    const Point2D &note_pos, const vector<std::shared_ptr<StringRect>> &rects,
    const ROMol &mol, const Bond *bond) const {
  string note = bond->getProp<string>(common_properties::bondNote);
  if (doesNoteClashNbourBonds(note_pos, rects, mol, bond->getBeginAtom())) {
    return 1;
  }
  unsigned int atom_idx = bond->getBeginAtomIdx();
  if (doesNoteClashAtomLabels(note_pos, rects, mol, atom_idx)) {
    return 2;
  }
  if (doesNoteClashOtherNotes(note_pos, rects)) {
    return 3;
  }
  return 0;
}

// ****************************************************************************
bool MolDraw2D::doesNoteClashNbourBonds(
    const Point2D &note_pos,
    const vector<std::shared_ptr<StringRect>> &rects, const ROMol &mol,
    const Atom *atom) const {

  double double_bond_offset = -1.0;
  Point2D const &at2_dcds =
      getDrawCoords(at_cds_[activeMolIdx_][atom->getIdx()]);

  double line_width = lineWidth() * scale() * 0.02;
  for (const auto &nbr : make_iterator_range(mol.getAtomNeighbors(atom))) {
    Point2D const &at1_dcds = getDrawCoords(at_cds_[activeMolIdx_][nbr]);
    if (text_drawer_->doesLineIntersect(rects, note_pos, at1_dcds,
                                        at2_dcds, line_width)) {
      return true;
    }
    // now see about clashing with other lines if not single
    auto bond = mol.getBondBetweenAtoms(atom->getIdx(), nbr);
    Bond::BondType bt = bond->getBondType();
    if (bt == Bond::SINGLE) {
      continue;
    }

    if (double_bond_offset < 0.0) {
      double_bond_offset = options_.multipleBondOffset;
      // mol files from, for example, Marvin use a bond length of 1 for just
      // about everything. When this is the case, the default multipleBondOffset
      // is just too much, so scale it back.
      if ((at1_dcds - at2_dcds).lengthSq() < 1.4 * scale()) {
        double_bond_offset *= 0.6;
      }
    }
    if (bt == Bond::DOUBLE || bt == Bond::AROMATIC || bt == Bond::TRIPLE) {
      Point2D l1s, l1f, l2s, l2f;
      if (bt == Bond::DOUBLE || bt == Bond::AROMATIC) {
        // use the atom coords for this to make sure the perp goes the
        // correct way (y coordinate issue).
        calcDoubleBondLines(mol, double_bond_offset, *bond,
                            at_cds_[activeMolIdx_][nbr],
                            at_cds_[activeMolIdx_][atom->getIdx()],
                            at_cds_[activeMolIdx_], l1s, l1f, l2s, l2f);
      } else {
        calcTripleBondLines(
            double_bond_offset, *bond, at_cds_[activeMolIdx_][nbr],
            at_cds_[activeMolIdx_][atom->getIdx()], l1s, l1f, l2s, l2f);
      }
      l1s = getDrawCoords(l1s);
      l1f = getDrawCoords(l1f);
      l2s = getDrawCoords(l2s);
      l2f = getDrawCoords(l2f);

      if (text_drawer_->doesLineIntersect(rects, note_pos, l1s, l1f,
                                          line_width) ||
          text_drawer_->doesLineIntersect(rects, note_pos, l2s, l2f,
                                          line_width)) {
        return true;
      }
    }
  }

  return false;
}

// ****************************************************************************
bool MolDraw2D::doesNoteClashAtomLabels(
    const Point2D &note_pos,
    const vector<std::shared_ptr<StringRect>> &rects, const ROMol &mol,
    unsigned int atom_idx) const {
  // try the atom_idx first as it's the most likely clash
  Point2D draw_cds = getDrawCoords(atom_idx);
  if (text_drawer_->doesStringIntersect(
          rects, note_pos, atom_syms_[activeMolIdx_][atom_idx].first,
          atom_syms_[activeMolIdx_][atom_idx].second, draw_cds)) {
    return true;
  }
  // if it's cluttered, it might clash with other labels.
  for (auto atom : mol.atoms()) {
    if (atom_idx == atom->getIdx()) {
      continue;
    }
    const auto &atsym = atom_syms_[activeMolIdx_][atom->getIdx()];
    if (atsym.first.empty()) {
      continue;
    }
    draw_cds = getDrawCoords(atom->getIdx());
    if (text_drawer_->doesStringIntersect(rects, note_pos, atsym.first,
                                          atsym.second, draw_cds)) {
      return true;
    }
  }

  return false;
}

// ****************************************************************************
bool MolDraw2D::doesNoteClashOtherNotes(
    const Point2D &note_pos,
    const vector<std::shared_ptr<StringRect>> &rects) const {
  for (auto const &annot : annotations_[activeMolIdx_]) {
    if (text_drawer_->doesRectIntersect(rects, note_pos, annot.rect_)) {
      return true;
    }
  }
  return false;
}

// ****************************************************************************
double MolDraw2D::getDrawLineWidth() const {
  double width = lineWidth();
  // This works fairly well for SVG and Cairo. 0.02 is picked by eye
  if (drawOptions().scaleBondWidth) {
    width *= scale() * 0.02;
    if (width < 0.0) {
      width = 0.0;
    }
  }
  return width;
}

// ****************************************************************************
// take the coords for atnum, with neighbour nbr_cds, and move cds out to
// accommodate the label associated with it.
void MolDraw2D::adjustBondEndForLabel(
    const std::pair<std::string, OrientType> &lbl, const Point2D &nbr_cds,
    Point2D &cds) const {
  if (lbl.first.empty()) {
    return;
  }

  Point2D draw_cds = getDrawCoords(cds);
  Point2D nbr_draw_cds = getDrawCoords(nbr_cds);

  text_drawer_->adjustLineForString(lbl.first, lbl.second, nbr_draw_cds,
                                    draw_cds);

  cds = getAtomCoords(make_pair(draw_cds.x, draw_cds.y));

  if (drawOptions().additionalAtomLabelPadding > 0.0) {
    // directionVector is normalised.
    Point2D bond =
        cds.directionVector(nbr_cds) * drawOptions().additionalAtomLabelPadding;
    cds += bond;
  }
}

// ****************************************************************************
pair<string, OrientType> MolDraw2D::getAtomSymbolAndOrientation(
    const Atom &atom) const {
  OrientType orient = getAtomOrientation(atom);
  string symbol = getAtomSymbol(atom, orient);

  return std::make_pair(symbol, orient);
}

std::string getAtomListText(const Atom &atom) {
  PRECONDITION(atom.hasQuery(), "no query");
  PRECONDITION(atom.getQuery()->getDescription() == "AtomOr", "bad query type");

  std::string res = "";
  if (atom.getQuery()->getNegation()) {
    res += "!";
  }
  res += "[";
  std::vector<int> vals;
  getAtomListQueryVals(atom.getQuery(), vals);
  for (unsigned int i = 0; i < vals.size(); ++i) {
    if (i != 0) {
      res += ",";
    }
    res += PeriodicTable::getTable()->getElementSymbol(vals[i]);
  }

  return res + "]";
}

// ****************************************************************************
string MolDraw2D::getAtomSymbol(const RDKit::Atom &atom,
                                OrientType orientation) const {
  if (drawOptions().noAtomLabels) {
    return "";
  }
  // adds XML-like annotation for super- and sub-script, in the same manner
  // as MolDrawing.py. My first thought was for a LaTeX-like system,
  // obviously...
  string symbol;
  bool literal_symbol = true;
  unsigned int iso = atom.getIsotope();
  if (drawOptions().atomLabels.find(atom.getIdx()) !=
      drawOptions().atomLabels.end()) {
    // specified labels are trump: no matter what else happens we will show
    // them.
    symbol = drawOptions().atomLabels.find(atom.getIdx())->second;
  } else if (atom.hasProp(common_properties::_displayLabel) ||
             atom.hasProp(common_properties::_displayLabelW)) {
    // logic here: if either _displayLabel or _displayLabelW is set, we will
    // definitely use one of those. if only one is set, we'll use that one if
    // both are set and the orientation is W then we'll use _displayLabelW,
    // otherwise _displayLabel

    std::string lbl;
    std::string lblw;
    atom.getPropIfPresent(common_properties::_displayLabel, lbl);
    atom.getPropIfPresent(common_properties::_displayLabelW, lblw);
    if (lbl.empty()) {
      lbl = lblw;
    }
    if (orientation == OrientType::W && !lblw.empty()) {
      symbol = lblw;
    } else {
      symbol = lbl;
    }
  } else if (atom.hasProp(common_properties::atomLabel)) {
    symbol = atom.getProp<std::string>(common_properties::atomLabel);
  } else if (drawOptions().dummiesAreAttachments && atom.getAtomicNum() == 0 &&
             atom.getDegree() == 1) {
    symbol = "";
    literal_symbol = false;
  } else if (isAtomListQuery(&atom)) {
    symbol = getAtomListText(atom);
  } else if (isComplexQuery(&atom)) {
    symbol = "?";
  } else if (drawOptions().atomLabelDeuteriumTritium &&
             atom.getAtomicNum() == 1 && (iso == 2 || iso == 3)) {
    symbol = ((iso == 2) ? "D" : "T");
    iso = 0;
  } else {
    literal_symbol = false;
    std::vector<std::string> preText, postText;

    // first thing after the symbol is the atom map
    if (atom.hasProp("molAtomMapNumber")) {
      string map_num = "";
      atom.getProp("molAtomMapNumber", map_num);
      postText.push_back(std::string(":") + map_num);
    }

    if (0 != atom.getFormalCharge()) {
      // charge always comes post the symbol
      int ichg = atom.getFormalCharge();
      string sgn = ichg > 0 ? string("+") : string("-");
      ichg = abs(ichg);
      if (ichg > 1) {
        sgn = std::to_string(ichg) + sgn;
      }
      // put the charge as a superscript
      postText.push_back(string("<sup>") + sgn + string("</sup>"));
    }

    int num_h = (atom.getAtomicNum() == 6 && atom.getDegree() > 0)
                    ? 0
                    : atom.getTotalNumHs();  // FIX: still not quite right

    if (drawOptions().explicitMethyl && atom.getAtomicNum() == 6 &&
        atom.getDegree() == 1) {
      symbol += atom.getSymbol();
      num_h = atom.getTotalNumHs();
    }

    if (num_h > 0 && !atom.hasQuery()) {
      // the H text comes after the atomic symbol
      std::string h = "H";
      if (num_h > 1) {
        // put the number as a subscript
        h += string("<sub>") + std::to_string(num_h) + string("</sub>");
      }
      postText.push_back(h);
    }

    if (0 != iso &&
        ((drawOptions().isotopeLabels && atom.getAtomicNum() != 0) ||
         (drawOptions().dummyIsotopeLabels && atom.getAtomicNum() == 0))) {
      // isotope always comes before the symbol
      preText.push_back(std::string("<sup>") + std::to_string(iso) +
                        std::string("</sup>"));
    }

    symbol = "";
    for (const std::string &se : preText) {
      symbol += se;
    }

    // allenes need a C, but extend to any atom with degree 2 and both
    // bonds in a line.
    if (isLinearAtom(atom, at_cds_[activeMolIdx_]) ||
        (atom.getAtomicNum() != 6 || atom.getDegree() == 0 || preText.size() ||
         postText.size())) {
      symbol += atom.getSymbol();
    }
    for (const std::string &se : postText) {
      symbol += se;
    }
  }

  if (literal_symbol && !symbol.empty()) {
    symbol = "<lit>" + symbol + "</lit>";
  }
  // cout << "Atom symbol " << atom.getIdx() << " : " << symbol << endl;
  return symbol;
}  // namespace RDKit

// ****************************************************************************
OrientType MolDraw2D::getAtomOrientation(const RDKit::Atom &atom) const {
  // cout << "Atomic " << atom.getAtomicNum() << " degree : "
  //      << atom.getDegree() << " : " << atom.getTotalNumHs() << endl;
  // anything with a slope of more than 70 degrees is vertical. This way,
  // the NH in an indole is vertical as RDKit lays it out normally (72ish
  // degrees) but the 2 amino groups of c1ccccc1C1CCC(N)(N)CC1 are E and W
  // when they are drawn at the bottom of the molecule.
  static const double VERT_SLOPE = tan(70.0 * M_PI / 180.0);

  auto &mol = atom.getOwningMol();
  const Point2D &at1_cds = at_cds_[activeMolIdx_][atom.getIdx()];
  Point2D nbr_sum(0.0, 0.0);
  // cout << "Nbours for atom : " << at1->getIdx() << endl;
  for (const auto &nbri : make_iterator_range(mol.getAtomBonds(&atom))) {
    const Bond *bond = mol[nbri];
    const Point2D &at2_cds =
        at_cds_[activeMolIdx_][bond->getOtherAtomIdx(atom.getIdx())];
    nbr_sum += at2_cds - at1_cds;
  }

  OrientType orient = OrientType::C;
  if (atom.getDegree()) {
    double islope = 1000.0;
    if (fabs(nbr_sum.x) > 1.0e-4) {
      islope = nbr_sum.y / nbr_sum.x;
    }
    if (fabs(islope) <= VERT_SLOPE) {
      if (nbr_sum.x > 0.0) {
        orient = OrientType::W;
      } else {
        orient = OrientType::E;
      }
    } else {
      if (nbr_sum.y > 0.0) {
        orient = OrientType::N;
      } else {
        orient = OrientType::S;
      }
    }
    // atoms of single degree should always be either W or E, never N or S.  If
    // either of the latter, make it E if the slope is close to vertical,
    // otherwise have it either as required.
    if (orient == OrientType::N || orient == OrientType::S) {
      if (atom.getDegree() == 1) {
        if (fabs(islope) > VERT_SLOPE) {
          orient = OrientType::E;
        } else {
          if (nbr_sum.x > 0.0) {
            orient = OrientType::W;
          } else {
            orient = OrientType::E;
          }
        }
      } else if (atom.getDegree() == 3) {
        // Atoms of degree 3 can sometimes have a bond pointing down with S
        // orientation or up with N orientation, which puts the H on the bond.
        auto &mol = atom.getOwningMol();
        const Point2D &at1_cds = at_cds_[activeMolIdx_][atom.getIdx()];
        for (const auto &nbri : make_iterator_range(mol.getAtomBonds(&atom))) {
          const Bond *bond = mol[nbri];
          const Point2D &at2_cds =
              at_cds_[activeMolIdx_][bond->getOtherAtomIdx(atom.getIdx())];
          Point2D bond_vec = at2_cds - at1_cds;
          double ang = atan(bond_vec.y / bond_vec.x) * 180.0 / M_PI;
          if (ang > 80.0 && ang < 100.0 && orient == OrientType::S) {
            orient = OrientType::N;
            break;
          } else if (ang < -80.0 && ang > -100.0 && orient == OrientType::N) {
            orient = OrientType::S;
            break;
          }
        }
      }
    }
  } else {
    // last check: degree zero atoms from the last three periods should have
    // the Hs first
    static int HsListedFirstSrc[] = {8, 9, 16, 17, 34, 35, 52, 53, 84, 85};
    std::vector<int> HsListedFirst(
        HsListedFirstSrc,
        HsListedFirstSrc + sizeof(HsListedFirstSrc) / sizeof(int));
    if (std::find(HsListedFirst.begin(), HsListedFirst.end(),
                  atom.getAtomicNum()) != HsListedFirst.end()) {
      orient = OrientType::W;
    } else {
      orient = OrientType::E;
    }
  }

  return orient;
}

// ****************************************************************************
void MolDraw2D::adjustScaleForAtomLabels(
    const std::vector<int> *highlight_atoms,
    const map<int, double> *highlight_radii) {
  double x_max(x_min_ + x_range_), y_max(y_min_ + y_range_);

  for (size_t i = 0; i < atom_syms_[activeMolIdx_].size(); ++i) {
    if (!atom_syms_[activeMolIdx_][i].first.empty()) {
      double this_x_min, this_y_min, this_x_max, this_y_max;
      getStringExtremes(atom_syms_[activeMolIdx_][i].first,
                        atom_syms_[activeMolIdx_][i].second,
                        at_cds_[activeMolIdx_][i], this_x_min, this_y_min,
                        this_x_max, this_y_max);
      x_max = std::max(x_max, this_x_max);
      x_min_ = std::min(x_min_, this_x_min);
      y_max = std::max(y_max, this_y_max);
      y_min_ = std::min(y_min_, this_y_min);
    }
    if (highlight_atoms &&
        highlight_atoms->end() !=
            find(highlight_atoms->begin(), highlight_atoms->end(), i)) {
      Point2D centre;
      double xradius, yradius;
      // this involves a 2nd call to text_drawer_->getStringRect, but never mind
      calcLabelEllipse(i, highlight_radii, centre, xradius, yradius);
      double this_x_min = centre.x - xradius;
      double this_x_max = centre.x + xradius;
      double this_y_min = centre.y - yradius;
      double this_y_max = centre.y + yradius;
      x_max = std::max(x_max, this_x_max);
      x_min_ = std::min(x_min_, this_x_min);
      y_max = std::max(y_max, this_y_max);
      y_min_ = std::min(y_min_, this_y_min);
    }
  }

  x_range_ = max(x_max - x_min_, x_range_);
  y_range_ = max(y_max - y_min_, y_range_);
}

// ****************************************************************************
void MolDraw2D::adjustScaleForRadicals(const ROMol &mol) {
  if (scale() != text_drawer_->fontScale()) {
    // we've hit max or min font size, so re-compute radical rectangles as
    // they'll be too far from the character.
    radicals_[activeMolIdx_].clear();
    extractRadicals(mol);
  }
  double x_max(x_min_ + x_range_), y_max(y_min_ + y_range_);

  for (auto rad_pair : radicals_[activeMolIdx_]) {
    auto rad_rect = rad_pair.first;
    x_max = max(x_max, rad_rect->trans_.x + rad_rect->width_ / 2.0);
    y_max = max(y_max, rad_rect->trans_.y + rad_rect->height_ / 2.0);
    x_min_ = min(x_min_, rad_rect->trans_.x - rad_rect->width_ / 2.0);
    y_min_ = min(y_min_, rad_rect->trans_.y - rad_rect->height_ / 2.0);
  }

  x_range_ = max(x_max - x_min_, x_range_);
  y_range_ = max(y_max - y_min_, y_range_);
}

// ****************************************************************************
void MolDraw2D::adjustScaleForAnnotation(const vector<AnnotationType> &notes) {
  double x_max(x_min_ + x_range_), y_max(y_min_ + y_range_);

  for (auto const &pr : notes) {
    const auto &note_rect = pr.rect_;
    double this_x_max = note_rect.trans_.x;
    double this_x_min = note_rect.trans_.x;
    double this_y_max = note_rect.trans_.y;
    double this_y_min = note_rect.trans_.y;
    if (pr.align_ == TextAlignType::START) {
      this_x_max += note_rect.width_;
    } else if (pr.align_ == TextAlignType::END) {
      this_x_min -= note_rect.width_;
    } else {
      this_x_max += note_rect.width_ / 2.0;
      this_x_min -= note_rect.width_ / 2.0;
    }
    this_y_max += note_rect.height_ / 2.0;
    this_y_min -= note_rect.height_ / 2.0;

    x_max = std::max(x_max, this_x_max);
    x_min_ = std::min(x_min_, this_x_min);
    y_max = std::max(y_max, this_y_max);
    y_min_ = std::min(y_min_, this_y_min);
  }
  x_range_ = max(x_max - x_min_, x_range_);
  y_range_ = max(y_max - y_min_, y_range_);
}

// ****************************************************************************
void MolDraw2D::drawTriangle(const Point2D &cds1, const Point2D &cds2,
                             const Point2D &cds3) {
  std::vector<Point2D> pts;
  if (!drawOptions().comicMode) {
    pts = {cds1, cds2, cds3};
  } else {
    auto lpts = MolDraw2D_detail::handdrawnLine(cds1, cds2, scale_);
    std::move(lpts.begin(), lpts.end(), std::back_inserter(pts));
    lpts = MolDraw2D_detail::handdrawnLine(cds2, cds3, scale_);
    std::move(lpts.begin(), lpts.end(), std::back_inserter(pts));
    lpts = MolDraw2D_detail::handdrawnLine(cds3, cds1, scale_);
    std::move(lpts.begin(), lpts.end(), std::back_inserter(pts));
  }
  drawPolygon(pts);
};

// ****************************************************************************
void MolDraw2D::drawArrow(const Point2D &arrowBegin, const Point2D &arrowEnd,
                          bool asPolygon, double frac, double angle) {
  Point2D delta = arrowBegin - arrowEnd;
  double cos_angle = std::cos(angle), sin_angle = std::sin(angle);

  Point2D p1 = arrowEnd;
  p1.x += frac * (delta.x * cos_angle + delta.y * sin_angle);
  p1.y += frac * (delta.y * cos_angle - delta.x * sin_angle);

  Point2D p2 = arrowEnd;
  p2.x += frac * (delta.x * cos_angle - delta.y * sin_angle);
  p2.y += frac * (delta.y * cos_angle + delta.x * sin_angle);

  drawLine(arrowBegin, arrowEnd);
  if (!asPolygon) {
    drawLine(arrowEnd, p1);
    drawLine(arrowEnd, p2);
  } else {
    std::vector<Point2D> pts = {p1, arrowEnd, p2};
    bool fps = fillPolys();
    setFillPolys(true);
    drawPolygon(pts);
    setFillPolys(fps);
  }
}

// ****************************************************************************
void MolDraw2D::tabulaRasa() {
  scale_ = 1.0;

  // ignore the min and max font sizes when setting font size to 1.0
  text_drawer_->setFontScale(1.0, true);
  x_trans_ = y_trans_ = 0.0;
  x_offset_ = y_offset_ = 0;
  d_metadata.clear();
  d_numMetadataEntries = 0;
  setActiveAtmIdx();
}

// ****************************************************************************
void MolDraw2D::drawEllipse(const Point2D &cds1, const Point2D &cds2) {
  std::vector<Point2D> pts;
  MolDraw2D_detail::arcPoints(cds1, cds2, pts, 0, 360);
  drawPolygon(pts);
}

// ****************************************************************************
void MolDraw2D::drawArc(const Point2D &centre, double radius, double ang1,
                        double ang2) {
  drawArc(centre, radius, radius, ang1, ang2);
}

// ****************************************************************************
void MolDraw2D::drawArc(const Point2D &centre, double xradius, double yradius,
                        double ang1, double ang2) {
  std::vector<Point2D> pts;
  // 5 degree increments should be plenty, as the circles are probably
  // going to be small.
  int num_steps = 1 + int((ang2 - ang1) / 5.0);
  double ang_incr = double((ang2 - ang1) / num_steps) * M_PI / 180.0;
  double start_ang_rads = ang2 * M_PI / 180.0;
  for (int i = 0; i <= num_steps; ++i) {
    double ang = start_ang_rads + double(i) * ang_incr;
    double x = centre.x + xradius * cos(ang);
    double y = centre.y + yradius * sin(ang);
    pts.emplace_back(Point2D(x, y));
  }

  if (fillPolys()) {
    // otherwise it draws an arc back to the pts.front() rather than filling
    // in the sector.
    pts.emplace_back(centre);
  }
  drawPolygon(pts);
}

// ****************************************************************************
void MolDraw2D::drawRect(const Point2D &cds1, const Point2D &cds2) {
  std::vector<Point2D> pts(4);
  pts[0] = cds1;
  pts[1] = Point2D(cds1.x, cds2.y);
  pts[2] = cds2;
  pts[3] = Point2D(cds2.x, cds1.y);
  // if fillPolys() is false, it doesn't close the polygon because of
  // its use for drawing filled or open ellipse segments.
  if (!fillPolys()) {
    pts.emplace_back(cds1);
  }
  drawPolygon(pts);
}

void MolDraw2D::drawWavyLine(const Point2D &cds1, const Point2D &cds2,
                             const DrawColour &col1, const DrawColour &col2,
                             unsigned int, double) {
  drawLine(cds1, cds2, col1, col2);
}

// ****************************************************************************
//  we draw the line at cds2, perpendicular to the line cds1-cds2
void MolDraw2D::drawAttachmentLine(const Point2D &cds1, const Point2D &cds2,
                                   const DrawColour &col, double len,
                                   unsigned int nSegments) {
  Point2D perp = calcPerpendicular(cds1, cds2);
  Point2D p1 = Point2D(cds2.x - perp.x * len / 2, cds2.y - perp.y * len / 2);
  Point2D p2 = Point2D(cds2.x + perp.x * len / 2, cds2.y + perp.y * len / 2);
  drawWavyLine(p1, p2, col, col, nSegments);
}

// ****************************************************************************
bool doLinesIntersect(const Point2D &l1s, const Point2D &l1f,
                      const Point2D &l2s, const Point2D &l2f, Point2D *ip) {
  // using spell from answer 2 of
  // https://stackoverflow.com/questions/563198/how-do-you-detect-where-two-line-segments-intersect
  double s1_x = l1f.x - l1s.x;
  double s1_y = l1f.y - l1s.y;
  double s2_x = l2f.x - l2s.x;
  double s2_y = l2f.y - l2s.y;

  double d = (-s2_x * s1_y + s1_x * s2_y);
  if (d == 0.0) {
    // parallel lines.
    return false;
  }
  double s, t;
  s = (-s1_y * (l1s.x - l2s.x) + s1_x * (l1s.y - l2s.y)) / d;
  t = (s2_x * (l1s.y - l2s.y) - s2_y * (l1s.x - l2s.x)) / d;

  if (s >= 0 && s <= 1 && t >= 0 && t <= 1) {
    if (ip) {
      ip->x = l1s.x + t * s1_x;
      ip->y = l1s.y + t * s1_y;
    }
    return true;
  }

  return false;
}

// ****************************************************************************
bool doesLineIntersectLabel(const Point2D &ls, const Point2D &lf,
                            const StringRect &lab_rect, double padding) {
  Point2D tl, tr, br, bl;
  lab_rect.calcCorners(tl, tr, br, bl, padding);

  // first check if line is completely inside label.  Unlikely, but who
  // knows?
  if (ls.x >= tl.x && ls.x <= br.x && lf.x >= tl.x && lf.x <= br.x &&
      ls.y <= tl.y && ls.y >= br.y && lf.y <= tl.y && lf.y >= br.y) {
    return true;
  }
  if (doLinesIntersect(ls, lf, tl, tr) || doLinesIntersect(ls, lf, tr, br) ||
      doLinesIntersect(ls, lf, br, bl) || doLinesIntersect(ls, lf, bl, tl)) {
    return true;
  }
  return false;
}

}  // namespace RDKit
