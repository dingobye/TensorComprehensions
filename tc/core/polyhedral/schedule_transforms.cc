/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "tc/core/polyhedral/schedule_transforms.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <glog/logging.h>

#include "tc/external/isl.h"

#include "tc/core/constants.h"
#include "tc/core/polyhedral/functional.h"
#include "tc/core/polyhedral/schedule_tree_elem.h"
#include "tc/core/polyhedral/schedule_tree_matcher.h"
#include "tc/core/scope_guard.h"
#include "tc/external/isl.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////
//                        Transformation functions, out-of-class
////////////////////////////////////////////////////////////////////////////////

namespace tc {
namespace polyhedral {
using namespace detail;

isl::union_map extendSchedule(
    const ScheduleTree* node,
    isl::union_map schedule) {
  if (auto bandElem = node->elemAs<ScheduleTreeElemBand>()) {
    if (bandElem->nMember() > 0) {
      schedule =
          schedule.flat_range_product(isl::union_map::from(bandElem->mupa_));
    }
  } else if (auto filterElem = node->elemAsBase<ScheduleTreeElemFilter>()) {
    schedule = schedule.intersect_domain(filterElem->filter_);
  } else if (auto extensionElem = node->elemAs<ScheduleTreeElemExtension>()) {
    // FIXME: we may need to restrict the range of reversed extension map to
    // schedule values that correspond to active domain elements at this
    // point.
    schedule = schedule.unite(
        extensionElem->extension_.reverse().intersect_range(schedule.range()));
  }

  return schedule;
}

namespace {
isl::union_map partialScheduleImpl(
    const ScheduleTree* root,
    const ScheduleTree* node,
    bool useNode) {
  auto schedule = isl::null<isl::union_map>();
  auto nodes = node->ancestors(root);
  if (useNode) {
    nodes.push_back(node);
  }
  for (auto anc : nodes) {
    if (auto domainElem = anc->elemAs<ScheduleTreeElemDomain>()) {
      schedule = isl::union_map::from_domain(domainElem->domain_);
    } else {
      schedule = extendSchedule(anc, schedule);
    }
  }
  return schedule;
}
} // namespace

isl::union_map prefixSchedule(
    const ScheduleTree* root,
    const ScheduleTree* node) {
  return partialScheduleImpl(root, node, false);
}

isl::union_map partialSchedule(
    const ScheduleTree* root,
    const ScheduleTree* node) {
  return partialScheduleImpl(root, node, true);
}

// Get a set of domain elements that are active at the given node.
//
// Domain elements are introduced by the root domain node.  Filter nodes
// disable the points that do not intersect with the filter.  Extension nodes
// are considered to introduce additional domain points.
isl::union_set activeDomainPoints(
    const ScheduleTree* root,
    const ScheduleTree* node) {
  auto domainElem = root->elemAs<ScheduleTreeElemDomain>();
  CHECK(domainElem) << "root must be a Domain node" << *root;

  auto domain = domainElem->domain_;
  if (root == node) {
    return domain;
  }

  for (auto anc : node->ancestors(root)) {
    if (auto filterElem = anc->elemAsBase<ScheduleTreeElemFilter>()) {
      domain = domain.intersect(filterElem->filter_);
    } else if (auto extensionElem = anc->elemAs<ScheduleTreeElemExtension>()) {
      auto parentSchedule = prefixSchedule(root, anc);
      auto extension = extensionElem->extension_;
      CHECK(parentSchedule.get() || extension.dim(isl::dim_type::in) == 0)
          << "expected a zero-dimensional domain of the Extension node "
             "in absence of parent band nodes";
      if (parentSchedule.get()) {
        parentSchedule = parentSchedule.intersect_domain(domain);
        domain = domain.unite(parentSchedule.range().apply(extension));
      } else {
        domain = domain.unite(extension.range());
      }
    }
  }
  return domain;
}

vector<ScheduleTree*> collectScheduleTreesPath(
    std::function<ScheduleTree*(ScheduleTree*)> next,
    ScheduleTree* start) {
  vector<ScheduleTree*> res{start};
  auto n = start;
  while ((n = next(n)) != nullptr) {
    res.push_back(n);
  }
  return res;
}

vector<const ScheduleTree*> collectScheduleTreesPath(
    std::function<const ScheduleTree*(const ScheduleTree*)> next,
    const ScheduleTree* start) {
  vector<const ScheduleTree*> res{start};
  auto n = start;
  while ((n = next(n)) != nullptr) {
    res.push_back(n);
  }
  return res;
}

// Replace "tree" in the list of its parent's children with newTree.
// Returns the pointer to newTree for call chaining purposes.
ScheduleTree* swapSubtree(
    ScheduleTree* relativeRoot,
    ScheduleTree* tree,
    ScheduleTreeUPtr& newTree) {
  CHECK(relativeRoot != tree) << "Need a strict relative root to graft";
  auto cpos = tree->positionRelativeTo(relativeRoot).back();
  auto parent = tree->ancestor(relativeRoot, 1);
  auto rawPtr = newTree.get();
  parent->swapChild(cpos, newTree);
  return rawPtr;
}

namespace {

/*
 * If the child of the band node "st" is also a band node,
 * then combine the two band nodes into a single band node
 * at the position of "st" and set "moveChildren" to true.
 * The coincident fields corresponding to the band members
 * that come from the nested band are reset, because the coincident
 * members of that nested band are only known to be coincident
 * within the outer band.
 */
ScheduleTree* joinBandsHelper(ScheduleTree* st, bool& moveChildren) {
  moveChildren = false;
  CHECK(st->elemAs<ScheduleTreeElemBand>());
  if (st->numChildren() != 1) {
    return st;
  }

  auto eb = st->elemAs<ScheduleTreeElemBand>();
  auto ebChild = st->child({0})->elemAs<ScheduleTreeElemBand>();
  if (!ebChild) {
    return st;
  }

  auto& partialSchedule = eb->mupa_;
  auto& partialScheduleChild = ebChild->mupa_;
  partialSchedule = partialSchedule.flat_range_product(partialScheduleChild);
  eb->coincident_.resize(
      eb->coincident_.size() + ebChild->coincident_.size(), false);
  eb->unroll_.insert(
      eb->unroll_.end(), ebChild->unroll_.begin(), ebChild->unroll_.end());

  moveChildren = true;
  return st;
}

} // namespace

ScheduleTree* joinBands(ScheduleTree* st, bool permutable) {
  bool moveChildren;
  st = joinBandsHelper(st, moveChildren);
  // Stupid private access hack, remove when moving to unique_ptr
  if (moveChildren) {
    // Just overwrite children and let shared pointers go out of scope
    auto children = st->detachChildren();
    CHECK_EQ(1, children.size()) << "expected a sequence of bands";
    st->appendChildren(children[0]->detachChildren());
  }
  st->elemAs<ScheduleTreeElemBand>()->permutable_ = permutable;
  return st;
}

ScheduleTree* joinBandsIterative(ScheduleTree* st, bool permutable) {
  bool moveChildren = true;
  while (moveChildren) {
    st = joinBandsHelper(st, moveChildren);
    // Stupid private access hack, remove when moving to unique_ptr
    if (moveChildren) {
      auto children = st->detachChildren();
      CHECK_EQ(1, children.size()) << "expected a sequence of bands";
      st->appendChildren(children[0]->detachChildren());
    }
  }
  st->elemAs<ScheduleTreeElemBand>()->permutable_ = permutable;
  return st;
}

using TileOptionsType = std::underlying_type<TileOptions>::type;

bool operator&(TileOptions actual, TileOptions wanted) {
  return static_cast<TileOptionsType>(actual) &
      static_cast<TileOptionsType>(wanted);
}

TileOptions operator|(TileOptions actual, TileOptions wanted) {
  return static_cast<TileOptions>(
      static_cast<TileOptionsType>(actual) |
      static_cast<TileOptionsType>(wanted));
}

// Note that by-reference ctx has only a semantic meaning: context will be
// changed by this call.
void applyTileOptions(isl::ctx& ctx, TileOptions tileOptions) {
  isl_options_set_tile_scale_tile_loops(
      ctx.get(), (tileOptions & TileOptions::ScaleTileLoops) ? 1 : 0);
  isl_options_set_tile_shift_point_loops(
      ctx.get(), (tileOptions & TileOptions::ShiftPointLoops) ? 1 : 0);
}

ScheduleTree*
bandSplit(ScheduleTree* relativeRoot, ScheduleTree* tree, size_t pos) {
  CHECK(tree->elemAs<ScheduleTreeElemBand>()) << "Not a band:\n" << *tree;
  auto band = tree->elemAs<ScheduleTreeElemBand>();
  size_t n = band->nMember();
  CHECK_LT(0, n) << "no bands to split";
  CHECK_LE(0, pos) << "position out of bounds";
  CHECK_GE(n, pos) << "position out of bounds";

  // Detach and reattach children to avoid making copies.
  auto children = tree->detachChildren();
  auto newChild = ScheduleTree::makeScheduleTree(*tree);
  newChild->appendChildren(std::move(children));
  auto newChildBand = newChild->elemAs<ScheduleTreeElemBand>();
  newChildBand->drop(0, pos);

  tree->appendChild(std::move(newChild));
  band->drop(pos, n - pos);
  return tree;
}

ScheduleTree*
bandSplitOut(ScheduleTree* relativeRoot, ScheduleTree* tree, size_t pos) {
  auto band = tree->elemAs<ScheduleTreeElemBand>();
  CHECK(band);
  auto schedule = band->mupa_;
  if (pos != schedule.dim(isl::dim_type::set) - 1) {
    tree = bandSplit(relativeRoot, tree, pos + 1);
  }
  if (pos != 0) {
    tree = bandSplit(relativeRoot, tree, pos);
    tree = tree->child({0});
  }
  return tree;
}

namespace {

template <typename T>
std::ostream& operator<<(ostream& os, const vector<T>& v) {
  for (auto vv : v) {
    os << vv << " ";
  }
  return os;
}
} // namespace

ScheduleTree* bandTile(
    ScheduleTree* st,
    const vector<size_t>& tileSizes,
    TileOptions tileOptions) {
  auto eb = st->elemAs<ScheduleTreeElemBand>();
  CHECK(eb) << "Not a band: " << *st;

  if (tileSizes.size() == 0) {
    return st;
  }
  auto& band = *eb;
  CHECK(band.permutable_) << "Can't tile an non-permutable band" << band;

  auto ts = tileSizes;
  if (band.nMember() > ts.size()) {
    ts.resize(band.nMember(), 0);
  }
  if (band.nMember() < ts.size()) {
    LOG(WARNING) << "Resizing tile sizes to " << band.nMember()
                 << " entries: " << ts;
    ts.resize(band.nMember());
  }
  CHECK_EQ(band.nMember(), ts.size()) << "NYI: incorrect sizes: " << ts;
  // TODO: adapt size
  // TODO: imperfectly nested loop tiling

  // Create a child, copy of st before outer tiling
  ScheduleTreeUPtr childUPtr = ScheduleTree::makeScheduleTree(*st);

  for (size_t i = 0;
       i < std::min(static_cast<size_t>(band.nMember()), ts.size());
       ++i) {
    auto upa = band.mupa_.get_union_pw_aff(i);
    if (ts[i]) {
      upa = upa.scale_down(isl::val(st->ctx_, ts[i])).floor();
      if (tileOptions & TileOptions::ScaleTileLoops) {
        upa = upa.scale_val(isl::val(st->ctx_, ts[i]));
      }
    } else {
      upa = upa.scale_val(isl::val(st->ctx_, ts[i]));
    }
    band.mupa_ = band.mupa_.set_union_pw_aff(i, upa);
  }

  auto ebChild = childUPtr->elemAs<ScheduleTreeElemBand>();
  CHECK(ebChild) << "Not a band: " << *childUPtr;
  auto& childBand = *ebChild;
  // No need for isl_schedule_band_point, it's almost done
  if (tileOptions & TileOptions::ShiftPointLoops) {
    auto mupa = band.mupa_;
    if (!(tileOptions & TileOptions::ScaleTileLoops)) {
      mupa = mupa.scale_multi_val(makeMultiVal(mupa.get_space(), ts));
    }
    childBand.mupa_ = childBand.mupa_.sub(mupa);
  }

  st->detachChildren(); // let 'em die
  st->appendChild(std::move(childUPtr));

  return st;
}

ScheduleTree* bandScale(ScheduleTree* tree, const vector<size_t>& scales) {
  auto eb = tree->elemAs<ScheduleTreeElemBand>();
  CHECK(eb) << "Not a band: " << *tree;
  auto& band = *eb;

  // This mimics the behavior of bandTile...
  auto s = scales;
  if (s.size() < band.nMember()) {
    s.resize(band.nMember(), 0);
  }
  if (band.nMember() < s.size()) {
    LOG_IF(INFO, FLAGS_debug_tc_mapper)
        << "Resizing scales to " << band.nMember() << " entries: " << s;
    s.resize(band.nMember());
  }
  auto& mupa = band.mupa_;
  auto space = mupa.get_space();
  mupa = mupa.scale_multi_val(isl::makeMultiVal(space, s));
  return tree;
}

namespace {

template <typename T>
vector<T> reversed(const vector<T>& vec) {
  vector<T> result;
  result.reserve(vec.size());
  result.insert(result.begin(), vec.rbegin(), vec.rend());
  return result;
}

template <typename T>
vector<const ScheduleTree*> filterType(const vector<const ScheduleTree*>& vec) {
  vector<const ScheduleTree*> result;
  for (auto e : vec) {
    if (e->elemAs<T>()) {
      result.push_back(e);
    }
  }
  return result;
}

template <typename T, typename Func>
T foldl(const vector<const ScheduleTree*> vec, Func op, T init = T()) {
  T value = init;
  for (auto st : vec) {
    value = op(st, value);
  }
  return value;
}

template <typename... Args>
ostream& operator<<(ostream& os, const vector<Args...>& v) {
  os << "[";
  bool first = true;
  for (auto const& ve : v) {
    if (!first) {
      os << ", ";
    }
    os << ve;
    first = true;
  }
  os << "]";
  return os;
}
} // namespace

isl::multi_union_pw_aff prefixScheduleMupa(
    const ScheduleTree* root,
    const ScheduleTree* tree) {
  auto domainElem = root->elemAs<ScheduleTreeElemDomain>();
  CHECK(domainElem);
  auto domain = domainElem->domain_.universe();
  auto zero = isl::multi_val::zero(domain.get_space().set_from_params());
  auto prefix = isl::multi_union_pw_aff(domain, zero);
  prefix = foldl(
      filterType<ScheduleTreeElemBand>(tree->ancestors(root)),
      [](const ScheduleTree* st, isl::multi_union_pw_aff prefix) {
        auto mupa = st->elemAs<ScheduleTreeElemBand>()->mupa_;
        return prefix.flat_range_product(mupa);
      },
      prefix);
  return prefix;
}

ScheduleTree* insertBandAbove(
    ScheduleTree* root,
    ScheduleTree* tree,
    isl::multi_union_pw_aff mupa) {
  auto parent = tree->ancestor(root, 1);
  auto childPos = tree->positionInParent(parent);
  auto child = parent->detachChild(childPos);
  parent->insertChild(childPos, ScheduleTree::makeBand(mupa, std::move(child)));
  return parent->child({childPos});
}

ScheduleTree* insertBandBelow(
    detail::ScheduleTree* tree,
    isl::multi_union_pw_aff mupa) {
  auto numChildren = tree->numChildren();
  CHECK_LE(numChildren, 1);
  tree->appendChild(ScheduleTree::makeBand(mupa, tree->detachChildren()));
  return tree->child({0});
}

void updateTopLevelContext(detail::ScheduleTree* root, isl::set context) {
  if (!matchOne(tc::polyhedral::domain(tc::polyhedral::context(any())), root)) {
    root->appendChild(ScheduleTree::makeContext(
        isl::set::universe(context.get_space()), root->detachChildren()));
  }
  auto contextElem = const_cast<detail::ScheduleTreeElemContext*>(
      root->child({0})->elemAs<detail::ScheduleTreeElemContext>());
  CHECK(contextElem) << "Expected domain(context(any()))";
  contextElem->context_ = contextElem->context_ & context;
}

ScheduleTree* insertSequenceAbove(ScheduleTree* root, ScheduleTree* tree) {
  auto parent = tree->ancestor(root, 1);
  auto childPos = tree->positionInParent(parent);
  auto filter = activeDomainPoints(root, tree).universe();
  parent->insertChild(
      childPos,
      ScheduleTree::makeSequence(
          ScheduleTree::makeFilter(filter, parent->detachChild(childPos))));
  return parent->child({childPos});
}

ScheduleTree* insertExtensionAbove(
    ScheduleTree* root,
    ScheduleTree* tree,
    isl::union_map extension) {
  auto parent = tree->ancestor(root, 1);
  auto childPos = tree->positionInParent(parent);
  auto child = parent->detachChild(childPos);
  parent->insertChild(
      childPos, ScheduleTree::makeExtension(extension, std::move(child)));
  return parent->child({childPos});
}

namespace {
/*
 * Insert an empty extension node above "st" in a tree with the given root and
 * return a pointer to the inserted extension node.
 */
detail::ScheduleTree* insertEmptyExtensionAbove(
    ScheduleTree* root,
    ScheduleTree* st) {
  auto domain = root->elemAs<ScheduleTreeElemDomain>();
  CHECK(domain);
  auto space = domain->domain_.get_space();
  auto extension = isl::union_map::empty(space);
  return insertExtensionAbove(root, st, extension);
}
} // namespace

void insertExtensionLabelAt(
    ScheduleTree* root,
    ScheduleTree* seqNode,
    size_t pos,
    isl::id id) {
  auto extensionTree = seqNode->ancestor(root, 1);
  auto extensionNode =
      extensionTree->elemAs<detail::ScheduleTreeElemExtension>();
  if (!extensionNode) {
    extensionTree = insertEmptyExtensionAbove(root, seqNode);
    extensionNode = extensionTree->elemAs<detail::ScheduleTreeElemExtension>();
  }
  CHECK(extensionNode);
  CHECK(seqNode->elemAs<detail::ScheduleTreeElemSequence>());
  auto prefix = prefixScheduleMupa(root, extensionTree);
  auto scheduleSpace = prefix.get_space();
  auto space = scheduleSpace.params().set_from_params().set_tuple_id(
      isl::dim_type::set, id);
  auto extensionSpace = scheduleSpace.map_from_domain_and_range(space);
  auto extension = isl::map::universe(extensionSpace);
  extensionNode->extension_ = extensionNode->extension_.unite(extension);
  auto filterNode = detail::ScheduleTree::makeFilter(extension.range());
  seqNode->insertChild(pos, std::move(filterNode));
}

void insertExtensionLabelBefore(
    ScheduleTree* root,
    ScheduleTree* tree,
    isl::id id) {
  size_t pos;
  auto parent = tree->ancestor(root, 1);
  ScheduleTree* seqTree;
  if (tree->elemAs<detail::ScheduleTreeElemSequence>()) {
    seqTree = tree;
    pos = 0;
  } else if (
      parent->elemAs<detail::ScheduleTreeElemFilter>() &&
      parent->ancestor(root, 1)->elemAs<detail::ScheduleTreeElemSequence>()) {
    seqTree = parent->ancestor(root, 1);
    pos = parent->positionInParent(seqTree);
  } else {
    seqTree = insertSequenceAbove(root, tree);
    pos = 0;
  }
  insertExtensionLabelAt(root, seqTree, pos, id);
}

void insertExtensionLabelAfter(
    ScheduleTree* root,
    ScheduleTree* tree,
    isl::id id) {
  size_t pos;
  auto parent = tree->ancestor(root, 1);
  ScheduleTree* seqTree;
  if (tree->elemAs<detail::ScheduleTreeElemSequence>()) {
    seqTree = tree;
    pos = tree->numChildren();
  } else if (
      parent->elemAs<detail::ScheduleTreeElemFilter>() &&
      parent->ancestor(root, 1)->elemAs<detail::ScheduleTreeElemSequence>()) {
    seqTree = parent->ancestor(root, 1);
    pos = parent->positionInParent(seqTree) + 1;
  } else {
    seqTree = insertSequenceAbove(root, tree);
    pos = 1;
  }
  insertExtensionLabelAt(root, seqTree, pos, id);
}

namespace {

/*
 * Simplify the given tree inside the given context.
 *
 * In particular, simplify filters and the domains
 * of band node partial schedules.
 * Elements of a sequence that end up with an empty filter are removed.
 */
void gist(ScheduleTree* tree, isl::union_set context) {
  if (auto bandElem = tree->elemAs<ScheduleTreeElemBand>()) {
    bandElem->mupa_ = bandElem->mupa_.gist(context);
  } else if (auto filterElem = tree->elemAsBase<ScheduleTreeElemFilter>()) {
    filterElem->filter_ = filterElem->filter_.gist(context);
    if (filterElem->filter_.is_empty()) {
      tree->detachChildren();
    }
  }
  for (auto child : tree->children()) {
    gist(child, context);
  }
  if (tree->elemAs<ScheduleTreeElemSequence>()) {
    for (auto i = tree->numChildren(); i > 0; --i) {
      auto child = tree->child({i - 1});
      if (auto filterElem = child->elemAsBase<ScheduleTreeElemFilter>()) {
        if (filterElem->filter_.is_empty()) {
          tree->detachChild(i - 1);
        }
      }
    }
  }
}

/*
 * Create a filter node with the given filter and single child node,
 * after simplifying the child node in the context of the filter.
 */
ScheduleTreeUPtr gistedFilter(isl::union_set filter, ScheduleTreeUPtr child) {
  gist(child.get(), filter);
  return ScheduleTree::makeFilter(filter, std::move(child));
}

} // namespace

void orderBefore(
    ScheduleTree* root,
    ScheduleTree* tree,
    isl::union_set filter) {
  auto other = activeDomainPoints(root, tree).subtract(filter);
  auto seq = ScheduleTree::makeSequence(
      gistedFilter(filter, ScheduleTree::makeScheduleTree(*tree)));
  auto parent = tree->ancestor(root, 1);
  auto childPos = tree->positionInParent(parent);
  seq->appendChild(gistedFilter(other, parent->detachChild(childPos)));
  parent->insertChild(childPos, std::move(seq));
}

void orderAfter(ScheduleTree* root, ScheduleTree* tree, isl::union_set filter) {
  auto other = activeDomainPoints(root, tree).subtract(filter);
  auto seq = ScheduleTree::makeSequence(
      gistedFilter(filter, ScheduleTree::makeScheduleTree(*tree)));
  auto parent = tree->ancestor(root, 1);
  auto childPos = tree->positionInParent(parent);
  seq->insertChild(0, gistedFilter(other, parent->detachChild(childPos)));
  parent->insertChild(childPos, std::move(seq));
}

detail::ScheduleTree* mergeConsecutiveMappingFilters(
    detail::ScheduleTree* root,
    detail::ScheduleTree* node) {
  CHECK(
      root->elemAs<ScheduleTreeElemDomain>() ||
      root->elemAs<ScheduleTreeElemExtension>());
  bool changed = true;
  while (changed) {
    changed = false;
    auto filterNodes = detail::ScheduleTree::collect(
        node, detail::ScheduleTreeType::MappingFilter);

    for (auto f : filterNodes) {
      auto p = f->ancestor(root, 1);
      auto parentFilter = p->elemAs<ScheduleTreeElemMappingFilter>();
      if (!parentFilter) {
        continue;
      }
      auto filter = f->elemAs<ScheduleTreeElemMappingFilter>();
      auto merged = parentFilter->filter_ & filter->filter_;
      // We can only merge filters that have the same number of tuples
      if (merged.n_set() != parentFilter->filter_.n_set() ||
          merged.n_set() != filter->filter_.n_set()) {
        continue;
      }
      p->elemAs<ScheduleTreeElemMappingFilter>()->filter_ = merged;
      // const cast to replace in place rather than construct a new
      // ScheduleTree object (which would not be more functional-style anyway)
      auto& ids = const_cast<std::unordered_set<
          mapping::MappingId,
          typename mapping::MappingId::Hash>&>(
          p->elemAs<ScheduleTreeElemMappingFilter>()->mappingIds);
      for (auto id : filter->mappingIds) {
        CHECK_EQ(0, ids.count(id))
            << "Error when merging filters\n"
            << *f << "\nand\n"
            << *p << "\nid: " << id << " mapped in both!";
        ids.insert(id);
      }
      p->replaceChild(f->positionInParent(p), f->detachChild(0));
      changed = true;
      break;
    }
  }
  return node;
}

} // namespace polyhedral
} // namespace tc
