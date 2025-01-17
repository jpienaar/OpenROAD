/////////////////////////////////////////////////////////////////////////////
//
// BSD 3-Clause License
//
// Copyright (c) 2023, The Regents of the University of California
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

#include "RDLRouter.h"

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/graph/astar_search.hpp>
#include <boost/graph/lookup_edge.hpp>
#include <boost/polygon/polygon.hpp>
#include <limits>
#include <list>
#include <set>

#include "Utilities.h"
#include "odb/db.h"
#include "odb/dbTransform.h"
#include "pad/ICeWall.h"
#include "utl/Logger.h"

namespace pad {

class RDLRouterDistanceHeuristic
    : public boost::astar_heuristic<RDLRouter::GridGraph, int64_t>
{
 public:
  RDLRouterDistanceHeuristic(
      const std::map<RDLRouter::grid_vertex, odb::Point>& vertex_map,
      const std::vector<RDLRouter::grid_vertex>& predecessor,
      const RDLRouter::grid_vertex& start_vertex,
      const odb::Point& goal,
      float turn_penalty)
      : vertex_map_(vertex_map),
        predecessor_(predecessor),
        start_vertex_(start_vertex),
        goal_(goal),
        turn_penalty_(turn_penalty)
  {
  }
  int64_t operator()(RDLRouter::grid_vertex vt_next)
  {
    const auto& pt_next = vertex_map_.at(vt_next);

    const int64_t distance = RDLRouter::distance(goal_, pt_next);

    const auto& vt_curr = predecessor_[vt_next];
    if (start_vertex_ == vt_curr) {
      return distance;
    }

    const auto& vt_prev = predecessor_[vt_curr];
    if (start_vertex_ == vt_prev) {
      return distance;
    }

    const auto& pt_curr = vertex_map_.at(vt_curr);
    const auto& pt_prev = vertex_map_.at(vt_prev);

    const odb::Point incoming_vec(pt_curr.x() - pt_prev.x(),
                                  pt_curr.y() - pt_prev.y());
    const odb::Point outgoing_vec(pt_next.x() - pt_curr.x(),
                                  pt_next.y() - pt_curr.y());

    int64_t penalty = 0;
    if (incoming_vec != outgoing_vec) {
      penalty = turn_penalty_ * RDLRouter::distance(pt_prev, pt_curr);
    }

    return distance + penalty;
  }

 private:
  const std::map<RDLRouter::grid_vertex, odb::Point>& vertex_map_;
  const std::vector<RDLRouter::grid_vertex>& predecessor_;
  const RDLRouter::grid_vertex& start_vertex_;
  odb::Point goal_;
  const float turn_penalty_;
};

struct RDLRouterGoalFound
{
};  // exception for termination

// visitor that terminates when we find the goal
template <class Vertex>
class RDLRouterGoalVisitor : public boost::default_astar_visitor
{
 public:
  explicit RDLRouterGoalVisitor(RDLRouter::grid_vertex goal) : goal_(goal) {}
  template <class Graph>
  void examine_vertex(Vertex u, Graph& g)
  {
    if (u == goal_) {
      throw RDLRouterGoalFound();
    }
  }

 private:
  RDLRouter::grid_vertex goal_;
};

RDLRouter::RDLRouter(utl::Logger* logger,
                     odb::dbBlock* block,
                     odb::dbTechLayer* layer,
                     odb::dbTechVia* bump_via,
                     odb::dbTechVia* pad_via,
                     const std::map<odb::dbITerm*, odb::dbITerm*>& routing_map,
                     int width,
                     int spacing,
                     bool allow45,
                     float turn_penalty)
    : logger_(logger),
      block_(block),
      layer_(layer),
      bump_accessvia_(bump_via),
      pad_accessvia_(pad_via),
      width_(width),
      spacing_(spacing),
      allow45_(allow45),
      turn_penalty_(turn_penalty),
      routing_map_(routing_map),
      gui_(nullptr)
{
  if (width_ == 0) {
    width_ = layer_->getWidth();
  }
  if (width_ < layer_->getWidth()) {
    const double dbus = block_->getDbUnitsPerMicron();
    logger_->warn(
        utl::PAD,
        3,
        "{:.3f}um is below the minimum width for {}, changing to {:.3f}um",
        width_ / dbus,
        layer_->getName(),
        layer_->getWidth() / dbus);
    width_ = layer_->getWidth();
  }
  if (spacing_ == 0) {
    spacing_ = layer_->getSpacing(width_);
  }
  if (spacing_ < layer_->getSpacing(width_)) {
    const double dbus = block_->getDbUnitsPerMicron();
    logger_->warn(
        utl::PAD,
        4,
        "{:.3f}um is below the minimum spacing for {}, changing to {:.3f}um",
        spacing_ / dbus,
        layer_->getName(),
        layer_->getSpacing(width_) / dbus);
    spacing_ = layer_->getSpacing(width_);
  }
}

RDLRouter::~RDLRouter()
{
  if (gui_ != nullptr) {
    gui_->setRouter(nullptr);
  }
}

void RDLRouter::route(const std::vector<odb::dbNet*>& nets)
{
  for (auto* net : nets) {
    routing_terminals_[net] = generateRoutingPairs(net);
  }

  // Build obstructions
  populateObstructions(nets);

  // build graph
  makeGraph();

  if (gui_ != nullptr) {
    gui_->pause();
  }

  std::map<odb::dbNet*, std::vector<TargetPair*>> failed;

  struct RouteSet
  {
    TargetPair* points;
    odb::dbNet* net;
  };
  std::vector<RouteSet> ordered_nets;
  for (auto& [net, points] : routing_terminals_) {
    for (auto& pointset : points) {
      ordered_nets.push_back({&pointset, net});
    }
  }
  std::sort(
      ordered_nets.begin(),
      ordered_nets.end(),
      [](const RouteSet& r, const RouteSet& l) -> bool {
        return distance(r.points->target0.center, r.points->target1.center)
               < distance(l.points->target0.center, l.points->target1.center);
      });

  logger_->info(utl::PAD, 5, "Routing {} nets", nets.size());
  debugPrint(logger_,
             utl::PAD,
             "Router",
             1,
             "  with {} segments",
             ordered_nets.size());

  const double dbus = block_->getDbUnitsPerMicron();
  for (const auto& [points, net] : ordered_nets) {
    debugPrint(logger_,
               utl::PAD,
               "Router",
               2,
               "Routing {} ({:.3f}um, {:.3f}um) -> ({:.3f}um, {:.3f}um)",
               net->getName(),
               points->target0.center.x() / dbus,
               points->target0.center.y() / dbus,
               points->target1.center.x() / dbus,
               points->target1.center.y() / dbus);

    const auto added_edges0
        = insertTerminalVertex(points->target0, points->target1);
    const auto added_edges1
        = insertTerminalVertex(points->target1, points->target0);

    auto route = run(points->target0.center, points->target1.center);
    if (!route.empty()) {
      debugPrint(
          logger_, utl::PAD, "Router", 2, "Route segments {}", route.size());
      routes_[net].push_back({route, points->target0, points->target1});
      commitRoute(route);
      points->state = RouteState::SUCCESS;
    } else {
      failed[net].push_back(points);
      points->state = RouteState::FAILED;
    }

    if (gui_ != nullptr && logger_->debugCheck(utl::PAD, "Router", 2)) {
      gui_->pause();
    }

    removeTerminalEdges(added_edges0);
    removeTerminalEdges(added_edges1);
  }

  if (!failed.empty()) {
    logger_->warn(
        utl::PAD, 6, "Failed to route the following {} nets:", failed.size());
    for (const auto& [net, segments] : failed) {
      logger_->report("  {}", net->getName());
      for (const auto& segment : segments) {
        logger_->report("    {} -> {}",
                        segment->target0.terminal->getName(),
                        segment->target1.terminal->getName());
      }
    }
  }

  // smooth wire
  // write to DB
  for (const auto& [net, net_routes] : routes_) {
    net->destroySWires();
    for (const auto& [route, source, target] : net_routes) {
      writeToDb(net, route, source, target);
    }
  }

  if (!failed.empty()) {
    logger_->error(utl::PAD, 7, "Failed to route {} nets.", failed.size());
  }
}

void RDLRouter::removeTerminalEdges(const std::vector<Edge>& edges)
{
  for (const auto& [p0, p1] : edges) {
    boost::remove_edge(point_vertex_map_[p0], point_vertex_map_[p1], graph_);
  }
}

std::vector<RDLRouter::Edge> RDLRouter::insertTerminalVertex(
    const RouteTarget& target,
    const RouteTarget& source)
{
  struct GridSnap
  {
    int pos;
    int index;
  };

  const double dbus = block_->getDbUnitsPerMicron();

  auto snap = [](const int pos, const std::vector<int>& grid) -> GridSnap {
    int dist = std::numeric_limits<int>::max();
    for (int i = 0; i < grid.size(); i++) {
      const int p = grid[i];
      const int new_dist = std::abs(p - pos);
      if (new_dist < dist) {
        dist = new_dist;
      } else {
        return {grid[i - 1], i - 1};
      }
    }

    return {-1, -1};
  };

  const GridSnap x_snap = snap(target.center.x(), x_grid_);
  const GridSnap y_snap = snap(target.center.y(), y_grid_);

  const odb::Point snapped(x_snap.pos, y_snap.pos);

  debugPrint(logger_,
             utl::PAD,
             "Router_snap",
             1,
             "Snap ({}, {}) -> ({}, {})",
             target.center.x(),
             target.center.y(),
             snapped.x(),
             snapped.y());

  if (x_snap.index == -1 || y_snap.index == -1) {
    logger_->error(utl::PAD,
                   8,
                   "Unable to snap ({:.3f}um, {:.3f}um) to routing grid.",
                   target.center.x() / dbus,
                   target.center.y() / dbus);
  }

  addGraphVertex(snapped);

  odb::Rect iterm_box;
  target.shape.bloat(getBloatFactor(), iterm_box);

  const float route_dist = distance(target.center, source.center);

  auto add_snap_edge
      = [this, &source, &route_dist, &snapped](const std::vector<int>& grid,
                                               const int index,
                                               const int const_pos,
                                               const int dindex,
                                               const bool is_x,
                                               const int term_boundary,
                                               odb::Point& edge_snap) -> bool {
    int idx = index + dindex;
    bool found = false;
    while (0 <= idx && idx < grid.size()) {
      const int grid_pt = grid[idx];
      bool inside_terminal;
      if (dindex > 0) {
        inside_terminal = grid_pt <= term_boundary;
      } else {
        inside_terminal = term_boundary <= grid_pt;
      }

      if (!inside_terminal) {
        odb::Point pt;
        if (is_x) {
          pt = odb::Point(grid_pt, const_pos);
        } else {
          pt = odb::Point(const_pos, grid_pt);
        }

        auto find_vertex = point_vertex_map_.find(pt);
        if (find_vertex != point_vertex_map_.end()) {
          edge_snap = pt;
          found = true;
          break;
        }
      }
      idx += dindex;
    }

    debugPrint(logger_,
               utl::PAD,
               "Router_snap",
               2,
               "Finding snap point ({}, {}) with must be outside {} and is "
               "searching in {} direction by {} and start was {}.",
               snapped.x(),
               snapped.y(),
               term_boundary,
               is_x ? "x" : "y",
               dindex,
               grid[index]);

    if (!found) {
      return false;
    }

    const float weight_scale = std::min(distance(edge_snap, source.center),
                                        distance(snapped, source.center))
                               / route_dist;

    debugPrint(logger_,
               utl::PAD,
               "Router_snap",
               2,
               "Adding edge ({}, {}) -> ({}, {}) with scale {}",
               snapped.x(),
               snapped.y(),
               edge_snap.x(),
               edge_snap.y(),
               weight_scale);

    return addGraphEdge(snapped, edge_snap, weight_scale, false);
  };
  std::set<odb::Point> edge_points;
  odb::Point edge_pt;
  if (add_snap_edge(x_grid_,
                    x_snap.index,
                    snapped.y(),
                    -1,
                    true,
                    iterm_box.xMin(),
                    edge_pt)) {
    edge_points.insert(edge_pt);
  }
  if (add_snap_edge(x_grid_,
                    x_snap.index,
                    snapped.y(),
                    1,
                    true,
                    iterm_box.xMax(),
                    edge_pt)) {
    edge_points.insert(edge_pt);
  }
  if (add_snap_edge(y_grid_,
                    y_snap.index,
                    snapped.x(),
                    -1,
                    false,
                    iterm_box.yMin(),
                    edge_pt)) {
    edge_points.insert(edge_pt);
  }
  if (add_snap_edge(y_grid_,
                    y_snap.index,
                    snapped.x(),
                    1,
                    false,
                    iterm_box.yMax(),
                    edge_pt)) {
    edge_points.insert(edge_pt);
  }

  if (edge_points.empty()) {
    logger_->error(
        utl::PAD,
        9,
        "No edges added to routing grid to access ({:.3f}um, {:.3f}um).",
        target.center.x() / dbus,
        target.center.y() / dbus);
  }

  const odb::Point& target_pt = target.center;
  const auto& snap_v = point_vertex_map_[snapped];

  std::vector<Edge> added_edges;

  auto get_weight = [&source, &route_dist](const odb::Point& p0,
                                           const odb::Point& p1) -> float {
    return std::min(distance(p0, source.center), distance(p1, source.center))
           / route_dist;
  };

  addGraphVertex(target_pt);
  // Add edges to hit center
  for (const auto& pt : edge_points) {
    added_edges.push_back({pt, snapped});
    const odb::Rect edge_shape(pt, snapped);
    if (edge_shape.xMin() <= target_pt.x()
        && target_pt.x() <= edge_shape.xMax()) {
      // Remove horizontal edge
      const auto& vh = point_vertex_map_[pt];
      boost::remove_edge(snap_v, vh, graph_);
      // Add middle point vertex
      const odb::Point new_pt(target_pt.x(), pt.y());
      addGraphVertex(new_pt);
      // Add two new horizontal edges
      if (addGraphEdge(pt, new_pt, get_weight(pt, new_pt), false)) {
        added_edges.push_back({pt, new_pt});
      }
      if (addGraphEdge(new_pt, snapped, get_weight(new_pt, snapped), false)) {
        added_edges.push_back({new_pt, snapped});
      }
      // Add edge to target
      if (addGraphEdge(
              new_pt, target_pt, get_weight(new_pt, target_pt), false)) {
        added_edges.push_back({new_pt, target_pt});
      }
    } else if (edge_shape.yMin() <= target_pt.y()
               && target_pt.y() <= edge_shape.yMax()) {
      // Remove vertical edge
      const auto& vv = point_vertex_map_[pt];
      boost::remove_edge(snap_v, vv, graph_);
      // Add middle point vertex
      const odb::Point new_pt(pt.x(), target_pt.y());
      addGraphVertex(new_pt);
      // Add two new vertical edges
      if (addGraphEdge(pt, new_pt, get_weight(pt, new_pt), false)) {
        added_edges.push_back({pt, new_pt});
      }
      if (addGraphEdge(new_pt, snapped, get_weight(new_pt, snapped), false)) {
        added_edges.push_back({new_pt, snapped});
      }
      // Add edge to target
      if (addGraphEdge(
              new_pt, target_pt, get_weight(new_pt, target_pt), false)) {
        added_edges.push_back({new_pt, target_pt});
      }
    }
  }

  return added_edges;
}

void RDLRouter::uncommitRoute(
    const std::set<std::pair<odb::Point, odb::Point>>& route)
{
  for (const auto& [p0, p1] : route) {
    addGraphEdge(p0, p1);
  }
}

odb::Rect RDLRouter::getPointObstruction(const odb::Point& pt) const
{
  const int check_dist = width_ / 2 + spacing_ + 1;
  return odb::Rect(pt.x() - check_dist,
                   pt.y() - check_dist,
                   pt.x() + check_dist,
                   pt.y() + check_dist);
}

odb::Polygon RDLRouter::getEdgeObstruction(const odb::Point& pt0,
                                           const odb::Point& pt1) const
{
  const int check_dist = width_ / 2 + spacing_ + 1;

  const odb::Oct check_oct(pt0, pt1, 2 * check_dist);

  std::vector<odb::Point> points = check_oct.getPoints();

  if (check_oct.getDir() == odb::Oct::RIGHT) {
    points[1].setX(check_oct.getCenterLow().x() + check_dist);
    points[2].setY(check_oct.getCenterHigh().y() - check_dist);
    points[5].setX(check_oct.getCenterHigh().x() - check_dist);
    points[6].setY(check_oct.getCenterLow().y() + check_dist);
  } else {
    points[3].setY(check_oct.getCenterLow().y() + check_dist);
    points[4].setX(check_oct.getCenterHigh().x() + check_dist);
    points[7].setY(check_oct.getCenterHigh().y() - check_dist);
    points[8].setX(check_oct.getCenterLow().x() - check_dist);
    points[0] = points[8];
  }

  return points;
}

bool RDLRouter::is45DegreeEdge(const odb::Point& pt0,
                               const odb::Point& pt1) const
{
  return pt0.x() != pt1.x() && pt0.y() != pt1.y();
}

std::set<RDLRouter::grid_edge> RDLRouter::getVertexEdges(
    const grid_vertex& vertex) const
{
  std::set<grid_edge> edges;

  GridGraph::out_edge_iterator oit, oend;
  std::tie(oit, oend) = boost::out_edges(vertex, graph_);
  for (; oit != oend; oit++) {
    edges.insert(*oit);
  }
  GridGraph::in_edge_iterator iit, iend;
  std::tie(iit, iend) = boost::in_edges(vertex, graph_);
  for (; iit != iend; iit++) {
    edges.insert(*iit);
  }

  return edges;
}

std::set<std::pair<odb::Point, odb::Point>> RDLRouter::commitRoute(
    const std::vector<grid_vertex>& route)
{
  std::set<grid_edge> edges;
  for (const auto& v : route) {
    const auto v_edges = getVertexEdges(v);
    edges.insert(v_edges.begin(), v_edges.end());
  }

  // remove intersecting edges
  using Line = boost::geometry::model::segment<odb::Point>;
  auto handle_rect_edge
      = [this, &edges](const odb::Rect& rect, const grid_edge& edge) {
          const odb::Point& lpt0 = vertex_point_map_[edge.m_source];
          const odb::Point& lpt1 = vertex_point_map_[edge.m_target];
          if (boost::geometry::intersects(rect, Line(lpt0, lpt1))) {
            edges.insert(edge);
          }
        };

  for (const auto& v : route) {
    const odb::Point& pt = vertex_point_map_[v];

    const odb::Rect check_box = getPointObstruction(pt);

    for (auto itr = vertex_grid_tree_.qbegin(
             boost::geometry::index::intersects(check_box));
         itr != vertex_grid_tree_.qend();
         itr++) {
      for (const auto& edge : getVertexEdges(itr->second)) {
        handle_rect_edge(check_box, edge);
      }
    }
  }

  if (allow45_) {
    // remove intersecting edges on 45 degrees

    auto handle_poly_edge
        = [this, &edges](const odb::Polygon& poly, const grid_edge& edge) {
            const odb::Point& lpt0 = vertex_point_map_[edge.m_source];
            const odb::Point& lpt1 = vertex_point_map_[edge.m_target];
            if (boost::geometry::intersects(poly, Line(lpt0, lpt1))) {
              edges.insert(edge);
            }
          };

    for (std::size_t i = 2; i < route.size(); i++) {
      const odb::Point& pt0 = vertex_point_map_[route[i - 1]];
      const odb::Point& pt1 = vertex_point_map_[route[i]];

      if (!is45DegreeEdge(pt0, pt1)) {
        continue;
      }

      const odb::Polygon check_poly = getEdgeObstruction(pt0, pt1);

      for (auto itr = vertex_grid_tree_.qbegin(
               boost::geometry::index::intersects(check_poly));
           itr != vertex_grid_tree_.qend();
           itr++) {
        for (const auto& edge : getVertexEdges(itr->second)) {
          handle_poly_edge(check_poly, edge);
        }
      }
    }
  }

  std::set<std::pair<odb::Point, odb::Point>> removed_edges;
  for (const auto& edge : edges) {
    removed_edges.emplace(vertex_point_map_[edge.m_source],
                          vertex_point_map_[edge.m_target]);
    boost::remove_edge(edge, graph_);
  }
  return removed_edges;
}

std::vector<RDLRouter::grid_vertex> RDLRouter::run(const odb::Point& source,
                                                   const odb::Point& dest)
{
  const int N = boost::num_vertices(graph_);
  std::vector<grid_vertex> p(N);
  std::vector<int64_t> d(N);

  const grid_vertex& start = point_vertex_map_[source];
  const grid_vertex& goal = point_vertex_map_[dest];

  debugPrint(logger_,
             utl::PAD,
             "Router",
             1,
             "Route ({}, {}) -> ({}, {})",
             source.x(),
             source.y(),
             dest.x(),
             dest.y());

  try {
    // call astar named parameter interface
    boost::astar_search_tree(
        graph_,
        start,
        RDLRouterDistanceHeuristic(
            vertex_point_map_, p, start, dest, turn_penalty_),
        boost::predecessor_map(
            boost::make_iterator_property_map(
                p.begin(), boost::get(boost::vertex_index, graph_)))
            .distance_map(boost::make_iterator_property_map(
                d.begin(), boost::get(boost::vertex_index, graph_)))
            .visitor(RDLRouterGoalVisitor<grid_vertex>(goal)));
  } catch (const RDLRouterGoalFound&) {  // found a path to the goal
    std::list<grid_vertex> shortest_path;
    for (grid_vertex v = goal;; v = p[v]) {
      shortest_path.push_front(v);
      if (p[v] == v) {
        break;
      }
    }

    std::vector<grid_vertex> route(shortest_path.begin(), shortest_path.end());
    return route;
  }

  return {};
}

void RDLRouter::makeGraph()
{
  point_vertex_map_.clear();
  vertex_point_map_.clear();
  graph_.clear();

  graph_weight_ = boost::get(boost::edge_weight, graph_);

  std::vector<int> x_grid;
  std::vector<int> y_grid;

  odb::dbTrackGrid* tracks = block_->findTrackGrid(layer_);
  tracks->getGridX(x_grid);
  tracks->getGridY(y_grid);

  // filter grid points based on spacing requirements
  const int pitch = width_ + spacing_ - 1;
  const int start = width_ / 2 + 1;
  x_grid_.clear();
  for (const auto& x : x_grid) {
    bool add = false;
    if (x_grid_.empty()) {
      if (x >= start) {
        add = true;
      }
    } else {
      if (*x_grid_.rbegin() + pitch < x) {
        add = true;
      }
    }

    if (add) {
      x_grid_.push_back(x);
    }
  }
  y_grid_.clear();
  for (const auto& y : y_grid) {
    bool add = false;
    if (y_grid_.empty()) {
      if (y >= start) {
        add = true;
      }
    } else {
      if (*y_grid_.rbegin() + pitch < y) {
        add = true;
      }
    }

    if (add) {
      y_grid_.push_back(y);
    }
  }

  for (const auto& x : x_grid_) {
    for (const auto& y : y_grid_) {
      addGraphVertex(odb::Point(x, y));
    }
  }

  debugPrint(logger_,
             utl::PAD,
             "Router",
             1,
             "Added {} vertices to graph",
             boost::num_vertices(graph_));

  for (size_t i = 0; i < x_grid_.size(); i++) {
    for (size_t j = 0; j < y_grid_.size(); j++) {
      const odb::Point center(x_grid_[i], y_grid_[j]);

      if (j + 1 < y_grid_.size()) {
        addGraphEdge(center, {x_grid_[i], y_grid_[j + 1]});
      }
      if (j != 0) {
        addGraphEdge(center, {x_grid_[i], y_grid_[j - 1]});
      }
      if (i != 0) {
        addGraphEdge(center, {x_grid_[i - 1], y_grid_[j]});
      }
      if (i + 1 < x_grid_.size()) {
        addGraphEdge(center, {x_grid_[i + 1], y_grid_[j]});
      }

      if (allow45_) {
        if (i % 2 == 1 || j % 2 == 1) {
          // only do every other position
          continue;
        }
        if (i + 1 < x_grid_.size() && j + 1 < y_grid_.size()) {
          addGraphEdge(center, {x_grid_[i + 1], y_grid_[j + 1]});
        }
        if (i + 1 < x_grid_.size() && j != 0) {
          addGraphEdge(center, {x_grid_[i + 1], y_grid_[j - 1]});
        }
        if (i != 0 && j + 1 < y_grid_.size()) {
          addGraphEdge(center, {x_grid_[i - 1], y_grid_[j + 1]});
        }
        if (i != 0 && j != 0) {
          addGraphEdge(center, {x_grid_[i - 1], y_grid_[j - 1]});
        }
      }
    }
  }

  std::vector<GridValue> grid_tree;
  for (const auto& [point, vertex] : point_vertex_map_) {
    odb::Rect rect(point, point);
    for (const auto& edge : getVertexEdges(vertex)) {
      rect.merge(odb::Rect(vertex_point_map_[edge.m_source],
                           vertex_point_map_[edge.m_target]));
    }
    grid_tree.emplace_back(rect, vertex);
  }
  vertex_grid_tree_ = GridTree(grid_tree.begin(), grid_tree.end());

  debugPrint(logger_,
             utl::PAD,
             "Router",
             1,
             "Added {} edges to graph",
             boost::num_edges(graph_));
}

bool RDLRouter::isEdgeObstructed(const odb::Point& pt0,
                                 const odb::Point& pt1) const
{
  using Line = boost::geometry::model::segment<odb::Point>;
  const Line line(pt0, pt1);
  for (auto itr
       = obstructions_.qbegin(boost::geometry::index::intersects(line));
       itr != obstructions_.qend();
       itr++) {
    const ObsValue& obs = *itr;
    if (boost::geometry::intersects(line, std::get<1>(obs))) {
      return true;
    }
  }
  return false;
}

void RDLRouter::addGraphVertex(const odb::Point& point)
{
  auto idx = boost::add_vertex(graph_);
  debugPrint(logger_,
             utl::PAD,
             "Router_vertex",
             1,
             "Adding point ({}, {}) as vertex {}",
             point.x(),
             point.y(),
             idx);
  point_vertex_map_[point] = idx;
  vertex_point_map_[idx] = point;
}

bool RDLRouter::addGraphEdge(const odb::Point& point0,
                             const odb::Point& point1,
                             float edge_weight_scale,
                             bool check_obstructions)
{
  auto point0check = point_vertex_map_.find(point0);
  if (point0check == point_vertex_map_.end()) {
    debugPrint(logger_,
               utl::PAD,
               "Router_edge",
               1,
               "Failed to find vertex at ({}, {})",
               point0.x(),
               point0.y());
    return false;
  }
  auto point1check = point_vertex_map_.find(point1);
  if (point1check == point_vertex_map_.end()) {
    debugPrint(logger_,
               utl::PAD,
               "Router_edge",
               1,
               "Failed to find vertex at ({}, {})",
               point1.x(),
               point1.y());
    return false;
  }
  grid_vertex v0 = point0check->second;
  grid_vertex v1 = point1check->second;
  if (v0 == v1) {
    return false;
  }

  if (check_obstructions && isEdgeObstructed(point0, point1)) {
    debugPrint(logger_,
               utl::PAD,
               "Router_edge",
               1,
               "Failed to add edge ({}, {}) -> ({}, {}) intersects obstruction",
               point0.x(),
               point0.y(),
               point1.x(),
               point1.y());
    return false;
  }

  bool added;
  grid_edge edge;

  bool exists;
  boost::tie(edge, exists) = boost::lookup_edge(v0, v1, graph_);
  if (exists) {
    return true;
  }
  boost::tie(edge, exists) = boost::lookup_edge(v1, v0, graph_);
  if (exists) {
    return true;
  }

  boost::tie(edge, added) = boost::add_edge(v0, v1, graph_);
  if (!added) {
    return false;
  }

  const int64_t weight = edge_weight_scale * distance(point0, point1);

  debugPrint(logger_,
             utl::PAD,
             "Router_edge",
             1,
             "Adding edge from ({}, {}) to ({}, {}) with weight {}",
             point0.x(),
             point0.y(),
             point1.x(),
             point1.y(),
             weight);
  graph_weight_[edge] = weight;

  return true;
}

std::vector<std::pair<odb::Point, odb::Point>> RDLRouter::simplifyRoute(
    const std::vector<grid_vertex>& route) const
{
  std::vector<std::pair<odb::Point, odb::Point>> wire;

  enum class Direction
  {
    UNSET,
    HORIZONTAL,
    VERTICAL,
    ANGLE45,
    ANGLE135
  };

  auto get_direction
      = [](const odb::Point& s, const odb::Point& t) -> Direction {
    if (s.y() == t.y()) {
      return Direction::HORIZONTAL;
    }
    if (s.x() == t.x()) {
      return Direction::VERTICAL;
    }
    if (s.x() < t.x() && s.y() < t.y()) {
      return Direction::ANGLE45;
    }
    if (s.x() > t.x() && s.y() > t.y()) {
      return Direction::ANGLE45;
    }
    return Direction::ANGLE135;
  };

  wire.emplace_back(vertex_point_map_.at(route[0]),
                    vertex_point_map_.at(route[1]));
  Direction direction
      = get_direction(wire.begin()->first, wire.begin()->second);
  for (size_t i = 2; i < route.size(); i++) {
    odb::Point s = wire.rbegin()->second;
    odb::Point t = vertex_point_map_.at(route[i]);

    Direction segment_direction = get_direction(s, t);
    if (direction == segment_direction) {
      // Extend segment
      wire.rbegin()->second = t;
    } else {
      // Determine if extentions are needed
      int extention = width_ / 2;
      if (direction == Direction::HORIZONTAL
          && segment_direction == Direction::VERTICAL) {
        const odb::Point& prev_s = wire.rbegin()->first;
        if (prev_s.x() < s.x()) {
          wire.rbegin()->second.setX(s.x() + extention);
        } else {
          wire.rbegin()->second.setX(s.x() - extention);
        }
        if (s.y() < t.y()) {
          s.setY(s.y() - extention);
        } else {
          s.setY(s.y() + extention);
        }
      } else if (direction == Direction::VERTICAL
                 && segment_direction == Direction::HORIZONTAL) {
        const odb::Point& prev_s = wire.rbegin()->first;
        if (prev_s.y() < s.y()) {
          wire.rbegin()->second.setY(s.y() + extention);
        } else {
          wire.rbegin()->second.setY(s.y() - extention);
        }
        if (s.x() < t.x()) {
          s.setX(s.x() - extention);
        } else {
          s.setX(s.x() + extention);
        }
      }

      // Start new segment
      wire.emplace_back(s, t);

      direction = segment_direction;
    }
  }

  return wire;
}

odb::Rect RDLRouter::correctEndPoint(const odb::Rect& route,
                                     const bool is_horizontal,
                                     const odb::Rect& target) const
{
  const int route_width = is_horizontal ? route.dy() : route.dx();
  const int target_width = is_horizontal ? target.dy() : target.dx();

  if (route_width <= target_width) {
    // shape is already fully covered
    return route;
  }

  odb::Rect new_route = route;
  new_route.merge(target);

  return new_route;
}

void RDLRouter::writeToDb(odb::dbNet* net,
                          const std::vector<grid_vertex>& route,
                          const RouteTarget& source,
                          const RouteTarget& target)
{
  Utilities::makeSpecial(net);

  auto* swire = odb::dbSWire::create(net, odb::dbWireType::ROUTED);
  const auto simplified_route = simplifyRoute(route);
  for (size_t i = 0; i < simplified_route.size(); i++) {
    const auto& [s, t] = simplified_route[i];
    odb::Rect shape(s, t);
    shape.bloat(width_ / 2, shape);
    odb::dbSBox::Direction dir;
    if (s.x() == t.x()) {
      shape.set_ylo(shape.yMin() + width_ / 2);
      shape.set_yhi(shape.yMax() - width_ / 2);
      dir = odb::dbSBox::VERTICAL;
    } else if (s.y() == t.y()) {
      shape.set_xlo(shape.xMin() + width_ / 2);
      shape.set_xhi(shape.xMax() - width_ / 2);
      dir = odb::dbSBox::HORIZONTAL;
    } else {
      dir = odb::dbSBox::OCTILINEAR;
    }

    if (dir != odb::dbSBox::OCTILINEAR) {
      if (i == 0) {
        shape = correctEndPoint(shape, s.y() == t.y(), source.shape);
      } else if (i + 1 == simplified_route.size()) {
        shape = correctEndPoint(shape, s.y() == t.y(), target.shape);
      }
    }

    if (dir != odb::dbSBox::OCTILINEAR) {
      odb::dbSBox::create(swire,
                          layer_,
                          shape.xMin(),
                          shape.yMin(),
                          shape.xMax(),
                          shape.yMax(),
                          odb::dbWireShapeType::IOWIRE);
    } else {
      odb::dbSBox::create(swire,
                          layer_,
                          s.x(),
                          s.y(),
                          t.x(),
                          t.y(),
                          odb::dbWireShapeType::IOWIRE,
                          odb::dbSBox::OCTILINEAR,
                          width_);
    }
  }

  if (source.layer != layer_) {
    odb::dbTechVia* via = pad_accessvia_;
    if (source.terminal->getMTerm()->getMaster()->getType().isCover()) {
      via = bump_accessvia_;
    }
    odb::dbSBox::create(swire,
                        via,
                        source.center.x(),
                        source.center.y(),
                        odb::dbWireShapeType::IOWIRE);
  }
  if (target.layer != layer_) {
    odb::dbTechVia* via = pad_accessvia_;
    if (target.terminal->getMTerm()->getMaster()->getType().isCover()) {
      via = bump_accessvia_;
    }
    odb::dbSBox::create(swire,
                        via,
                        target.center.x(),
                        target.center.y(),
                        odb::dbWireShapeType::IOWIRE);
  }
}

int RDLRouter::getBloatFactor() const
{
  return width_ / 2 + spacing_;
}

std::set<odb::Polygon> RDLRouter::getITermShapes(odb::dbITerm* iterm) const
{
  std::set<odb::Polygon> polys;

  const odb::dbTransform xform = iterm->getInst()->getTransform();

  for (auto* mpin : iterm->getMTerm()->getMPins()) {
    for (auto* geom : mpin->getPolygonGeometry()) {
      if (geom->getTechLayer() != layer_) {
        continue;
      }

      odb::Polygon poly = geom->getPolygon();
      xform.apply(poly);
      polys.insert(poly);
    }
    for (auto* geom : mpin->getGeometry(false)) {
      if (geom->getTechLayer() != layer_) {
        continue;
      }

      odb::Rect rect = geom->getBox();
      xform.apply(rect);
      polys.insert(rect);
    }
  }

  return polys;
}

void RDLRouter::populateObstructions(const std::vector<odb::dbNet*>& nets)
{
  std::vector<ObsValue> obstructions;

  const int bloat = getBloatFactor();
  auto insert_obstruction_rect
      = [&obstructions, bloat](const odb::Rect& rect, odb::dbNet* net) {
          odb::Rect bloated;
          rect.bloat(bloat, bloated);

          obstructions.emplace_back(bloated, bloated, net);
        };
  auto insert_obstruction_oct = [&obstructions, bloat](const odb::Oct& oct,
                                                       odb::dbNet* net) {
    const odb::Oct bloat_oct = oct.bloat(bloat);

    obstructions.emplace_back(bloat_oct.getEnclosingRect(), bloat_oct, net);
  };
  auto insert_obstruction_poly
      = [&obstructions, bloat](const odb::Polygon& poly, odb::dbNet* net) {
          const odb::Polygon bloat_poly = poly.bloat(bloat);

          obstructions.emplace_back(
              bloat_poly.getEnclosingRect(), bloat_poly, net);
        };

  // Get placed instanced obstructions
  for (auto* inst : block_->getInsts()) {
    if (!inst->isPlaced()) {
      continue;
    }

    const odb::dbTransform xform = inst->getTransform();

    auto* master = inst->getMaster();
    for (auto* obs : master->getPolygonObstructions()) {
      if (obs->getTechLayer() != layer_) {
        continue;
      }

      odb::Polygon poly = obs->getPolygon();
      xform.apply(poly);
      insert_obstruction_poly(poly, nullptr);
    }
    for (auto* obs : master->getObstructions(false)) {
      if (obs->getTechLayer() != layer_) {
        continue;
      }

      odb::Rect rect = obs->getBox();
      xform.apply(rect);
      insert_obstruction_rect(rect, nullptr);
    }

    for (auto* iterm : inst->getITerms()) {
      auto* net = iterm->getNet();
      for (const auto& poly : getITermShapes(iterm)) {
        insert_obstruction_poly(poly, net);
      }
    }
  }

  // Get already routed nets obstructions, excluding those that will be routed
  // now
  for (auto* net : block_->getNets()) {
    if (std::find(nets.begin(), nets.end(), net) != nets.end()) {
      continue;
    }

    for (auto* swire : net->getSWires()) {
      for (auto* box : swire->getWires()) {
        if (box->getTechLayer() != layer_) {
          continue;
        }

        if (box->getDirection() == odb::dbSBox::OCTILINEAR) {
          insert_obstruction_oct(box->getOct(), net);
        } else {
          insert_obstruction_rect(box->getBox(), net);
        }
      }
    }
  }

  // Get routing obstructions
  for (auto* obs : block_->getObstructions()) {
    auto* box = obs->getBBox();
    if (box->getTechLayer() != layer_) {
      continue;
    }

    insert_obstruction_rect(box->getBox(), nullptr);
  }

  // Add via obstructions when using access vias
  for (const auto& [net, routing_pairs] : routing_terminals_) {
    for (const auto& [source, target, status] : routing_pairs) {
      if (source.layer != layer_) {
        insert_obstruction_rect(source.shape, net);
      }
      if (target.layer != layer_) {
        insert_obstruction_rect(target.shape, net);
      }
    }
  }

  obstructions_ = ObsTree(obstructions.begin(), obstructions.end());
}

int64_t RDLRouter::distance(const odb::Point& p0, const odb::Point& p1)
{
  const int64_t dx = p0.x() - p1.x();
  const int64_t dy = p0.y() - p1.y();
  return std::sqrt(dx * dx + dy * dy);
}

odb::dbTechLayer* RDLRouter::getOtherLayer(odb::dbTechVia* via) const
{
  if (via != nullptr) {
    if (via->getBottomLayer() != layer_) {
      return via->getBottomLayer();
    }
    if (via->getTopLayer() != layer_) {
      return via->getTopLayer();
    }
  }
  return nullptr;
}

std::vector<RDLRouter::TargetPair> RDLRouter::generateRoutingPairs(
    odb::dbNet* net) const
{
  std::map<odb::Rect, std::pair<odb::dbITerm*, odb::dbTechLayer*>> terms;
  odb::dbTechLayer* bump_pin_layer = getOtherLayer(bump_accessvia_);
  odb::dbTechLayer* pad_pin_layer = getOtherLayer(pad_accessvia_);

  for (auto* iterm : net->getITerms()) {
    if (!iterm->getInst()->isPlaced()) {
      continue;
    }

    const bool is_bump = iterm->getMTerm()->getMaster()->getType().isCover();
    odb::dbTechLayer* other_layer;
    odb::dbTechVia* via;
    if (is_bump) {
      other_layer = bump_pin_layer;
      via = bump_accessvia_;
    } else {
      other_layer = pad_pin_layer;
      via = pad_accessvia_;
    }

    const odb::dbTransform xform = iterm->getInst()->getTransform();

    bool found = false;
    for (auto* mpin : iterm->getMTerm()->getMPins()) {
      for (auto* geom : mpin->getGeometry()) {
        odb::dbTechLayer* found_layer = geom->getTechLayer();
        if (found_layer != layer_ && found_layer != other_layer) {
          continue;
        }

        odb::Rect box = geom->getBox();
        if (found_layer == other_layer) {
          for (const auto& viabox : via->getBoxes()) {
            if (viabox->getTechLayer() == other_layer) {
              odb::Rect via_encl = viabox->getBox();
              via_encl.moveDelta(box.xCenter(), box.yCenter());
              box = via_encl;
              break;
            }
          }
        }
        xform.apply(box);

        terms[box] = {iterm, found_layer};
        found = true;
        break;
      }
      if (found) {
        break;
      }
    }
  }

  if (terms.size() < 2) {
    logger_->error(utl::PAD,
                   10,
                   "{} only has one iterm on {} layer",
                   net->getName(),
                   layer_->getName());
  }

  debugPrint(logger_,
             utl::PAD,
             "Router",
             1,
             "{} has {} terminals",
             net->getName(),
             terms.size());

  const double dbus = block_->getDbUnitsPerMicron();
  std::vector<TargetPair> pairs;
  if (terms.size() == 2) {
    const auto& [shape0, term0] = *terms.begin();
    const auto& [shape1, term1] = *terms.rbegin();
    pairs.push_back({{shape0.center(), shape0, term0.first, term0.second},
                     {shape1.center(), shape1, term1.first, term1.second}});
  } else {
    std::set<odb::dbInst*> used_instances;
    std::set<odb::Rect> used;
    for (const auto& [shape0, iterm0] : terms) {
      if (used.find(shape0) != used.end()) {
        continue;
      }
      if (used_instances.find(iterm0.first->getInst())
          != used_instances.end()) {
        continue;
      }

      // only pick covers (bumps)
      if (!iterm0.first->getMTerm()->getMaster()->getType().isCover()) {
        continue;
      }

      debugPrint(logger_,
                 utl::PAD,
                 "Router",
                 2,
                 "Finding routing pair for {} ({})",
                 iterm0.first->getName(),
                 iterm0.first->getNet()->getName());

      odb::dbITerm* find_terminal = nullptr;
      auto check_routing_map = routing_map_.find(iterm0.first);
      if (check_routing_map != routing_map_.end()) {
        find_terminal = check_routing_map->second;

        if (find_terminal == nullptr) {
          // do not route this bump
          used.insert(shape0);
          continue;
        }
      }

      int64_t dist = std::numeric_limits<int64_t>::max();
      const odb::Point pt0 = shape0.center();
      odb::Rect shape = shape0;
      odb::Point point = pt0;
      odb::dbITerm* term = iterm0.first;
      odb::dbTechLayer* layer = iterm0.second;
      for (const auto& [shape1, iterm1] : terms) {
        if (used.find(shape1) != used.end() || shape0 == shape1) {
          continue;
        }
        if (used_instances.find(iterm1.first->getInst())
            != used_instances.end()) {
          continue;
        }

        if (find_terminal != nullptr) {
          if (find_terminal != iterm1.first) {
            continue;
          }
        } else if (iterm1.first->getMTerm()->getMaster()->getType().isCover()) {
          // only pick non covers
          continue;
        }

        const odb::Point pt1 = shape1.center();
        const int64_t new_dist = distance(pt0, pt1);

        debugPrint(logger_,
                   utl::PAD,
                   "Router",
                   2,
                   "  {} ({}): {:.4f}um",
                   iterm1.first->getName(),
                   iterm1.first->getNet()->getName(),
                   new_dist / dbus);

        if (new_dist < dist) {
          dist = new_dist;
          shape = shape1;
          point = pt1;
          term = iterm1.first;
          layer = iterm1.second;
        }
      }

      if (pt0 == point) {
        logger_->error(utl::PAD,
                       37,
                       "Unable to find routing pair for {} ({})",
                       iterm0.first->getName(),
                       iterm0.first->getNet()->getName());
      }

      used.insert(shape0);
      used_instances.insert(iterm0.first->getInst());
      if (!find_terminal) {
        used.insert(shape);
        used_instances.insert(term->getInst());
      }
      pairs.push_back({{pt0, shape0, iterm0.first, iterm0.second},
                       {point, shape, term, layer}});
    }
  }
  return pairs;
}

/////////////////////////////////////

RDLGui::RDLGui()
{
  addDisplayControl(draw_vertex_, true);
  addDisplayControl(draw_edge_, true);
  addDisplayControl(draw_obs_, true);
  addDisplayControl(draw_fly_wires_, true);
  addDisplayControl(draw_routes_, true);
  addDisplayControl(draw_route_obstructions_, true);
}

RDLGui::~RDLGui()
{
  if (router_ != nullptr) {
    router_->setRDLGui(nullptr);
  }
}

void RDLGui::drawObjects(gui::Painter& painter)
{
  if (router_ == nullptr) {
    return;
  }
  const bool draw_detail = painter.getPixelsPerDBU() * 1000 >= 1;

  const odb::Rect box = painter.getBounds();

  const auto& vertex_map = router_->getVertexMap();

  std::map<odb::dbITerm*, const RDLRouter::NetRoute*> routes;
  for (const auto& [net, net_routes] : router_->getRoutes()) {
    for (const auto& route : net_routes) {
      routes[route.source.terminal] = &route;
    }
  }

  const bool draw_obs = draw_detail && checkDisplayControl(draw_obs_);
  if (draw_obs) {
    gui::Painter::Color obs_color = gui::Painter::cyan;
    obs_color.a = 127;
    painter.setPenAndBrush(obs_color, true);

    for (const auto& [rect, poly, ptr] : router_->getObstructions()) {
      painter.drawPolygon(poly);
    }
  }

  const bool draw_vertex = draw_detail && checkDisplayControl(draw_vertex_);
  const bool draw_edge = draw_detail && checkDisplayControl(draw_edge_);

  std::vector<RDLRouter::GridGraph::vertex_descriptor> vertex;
  if (draw_vertex || draw_edge) {
    RDLRouter::GridGraph::vertex_iterator v, vend;
    for (boost::tie(v, vend) = boost::vertices(router_->getGraph()); v != vend;
         ++v) {
      const odb::Point& pt = vertex_map.at(*v);
      if (box.contains({pt, pt})) {
        vertex.push_back(*v);
      }
    }
  }

  if (draw_vertex) {
    painter.setPenAndBrush(gui::Painter::red, true);

    for (const auto& v : vertex) {
      const odb::Point& pt = vertex_map.at(v);
      painter.drawCircle(pt.x(), pt.y(), 100);
    }
  }

  if (draw_edge) {
    gui::Painter::Color edge_color = gui::Painter::green;
    edge_color.a = 127;
    painter.setPenAndBrush(edge_color, true);

    for (const auto& v : vertex) {
      RDLRouter::GridGraph::out_edge_iterator eit, eend;
      std::tie(eit, eend) = boost::out_edges(v, router_->getGraph());
      for (; eit != eend; eit++) {
        const odb::Point& pt0 = vertex_map.at(eit->m_source);
        const odb::Point& pt1 = vertex_map.at(eit->m_target);
        painter.drawLine(pt0, pt1);
      }
    }
  }

  const bool draw_flywires = checkDisplayControl(draw_fly_wires_);
  if (draw_flywires) {
    const gui::Painter::Color success = gui::Painter::green;
    const gui::Painter::Color failed = gui::Painter::red;
    const gui::Painter::Color pending = gui::Painter::yellow;

    for (const auto& [net, pairs] : router_->getRoutingMap()) {
      for (const auto& pair : pairs) {
        switch (pair.state) {
          case RDLRouter::RouteState::PENDING:
            painter.setPenAndBrush(
                pending, true, gui::Painter::Brush::SOLID, 3);
            break;
          case RDLRouter::RouteState::FAILED:
            painter.setPenAndBrush(failed, true, gui::Painter::Brush::SOLID, 3);
            break;
          case RDLRouter::RouteState::SUCCESS:
            painter.setPenAndBrush(
                success, true, gui::Painter::Brush::SOLID, 3);
            break;
        }
        painter.drawLine(pair.target0.center, pair.target1.center);
      }
    }
  }

  if (checkDisplayControl(draw_routes_)) {
    painter.setPenAndBrush(
        gui::Painter::green, true, gui::Painter::Brush::SOLID, 3);

    for (const auto& [iterm, route] : routes) {
      for (size_t i = 1; i < route->route.size(); i++) {
        const odb::Point& src = vertex_map.at(route->route.at(i - 1));
        const odb::Point& dst = vertex_map.at(route->route.at(i));

        painter.drawLine(src, dst);
      }
    }
  }

  if (checkDisplayControl(draw_route_obstructions_)) {
    for (const auto& [iterm, route] : routes) {
      for (size_t i = 1; i < route->route.size(); i++) {
        const odb::Point& src = vertex_map.at(route->route.at(i - 1));
        const odb::Point& dst = vertex_map.at(route->route.at(i));

        painter.setPenAndBrush(
            gui::Painter::green, true, gui::Painter::Brush::NONE, 2);
        if (i == 1) {
          painter.drawRect(router_->getPointObstruction(src));
        }
        painter.drawRect(router_->getPointObstruction(dst));
        if (router_->is45DegreeEdge(src, dst)) {
          painter.drawPolygon(router_->getEdgeObstruction(src, dst));
        }
      }
    }
  }
}

void RDLGui::setRouter(RDLRouter* router)
{
  router_ = router;
  if (router_) {
    router_->setRDLGui(this);
  }
}

void RDLGui::pause()
{
  gui::Gui::get()->redraw();
  gui::Gui::get()->pause();
}

}  // namespace pad
