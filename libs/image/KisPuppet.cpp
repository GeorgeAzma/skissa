/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPuppet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <QPolygonF>

#include <KoColorSpace.h>

#include "KisGridRenderer.h"
#include "kis_four_point_interpolator_backward.h"
#include "kis_paint_device.h"

namespace {

// how much the air between painted cells resists bending, against 1 for paint
const double airStiffness = 1e-3;
// each separate shape (island) moves as its own pins say: without pins it stays, with one it
// moves without turning, with more it bends. The air holds lightly to its nearest island's movement.
const double airAnchor = 1e-4;
const double heldAnchor = 1.0;
// how hard a pin holds its point
const double pinWeight = 1e5;
// keeps the systems solvable without pulling anything noticeably
const double settle = 1e-7;

using Sparse = Eigen::SparseMatrix<double>;
using Solver = Eigen::SimplicialLDLT<Sparse>;

struct Triangle {
    int v[3];
    double weight;
    // each corner as seen from the other two at rest: corner 2 = 0 + x (1 - 0) + y (1 - 0) turned a quarter
    double x[3];
    double y[3];
};

struct Pin {
    QPointF rest;
    QPointF at;
    int corners[4];
    double weights[4];
    int island;
};

/// Turns and moves (never scales) points onto others as closely as can be: a move for one point
struct Motion {
    QPointF fromCenter;
    QPointF toCenter;
    double c {1};
    double s {0};

    static Motion fit(const QVector<QPointF> &from, const QVector<QPointF> &to)
    {
        Motion m;
        if (from.isEmpty()) return m;
        for (int i = 0; i < from.size(); i++) {
            m.fromCenter += from[i];
            m.toCenter += to[i];
        }
        m.fromCenter /= from.size();
        m.toCenter /= from.size();
        double along = 0;
        double across = 0;
        for (int i = 0; i < from.size(); i++) {
            const QPointF a = from[i] - m.fromCenter;
            const QPointF b = to[i] - m.toCenter;
            along += a.x() * b.x() + a.y() * b.y();
            across += a.x() * b.y() - a.y() * b.x();
        }
        const double turn = std::atan2(across, along);
        m.c = std::cos(turn);
        m.s = std::sin(turn);
        return m;
    }

    QPointF map(const QPointF &p) const
    {
        const QPointF a = p - fromCenter;
        return toCenter + QPointF(c * a.x() - s * a.y(), s * a.x() + c * a.y());
    }
};

}

struct KisPuppet::Private {
    QRect bounds;
    int step {16};
    int columns {0};
    int rows {0};
    std::vector<bool> stiff;
    QVector<QPointF> rest;
    QVector<QPointF> bent;
    std::vector<Triangle> triangles;
    std::vector<Pin> pins;
    // the separate shapes: each corner's own island (-1 in the air) and the nearest one
    int islands {0};
    std::vector<int> island;
    std::vector<int> nearest;
    // how firmly each corner holds to its island's movement
    std::vector<double> anchor;

    bool factored {false};
    Solver scaleFree;
    Solver fitted;

    int corner(int column, int row) const { return row * columns + column; }

    /// The cell holding \p point and the bilinear weights of its corners
    void cornersOf(const QPointF &point, int *corners, double *weights) const
    {
        const double fx = (point.x() - bounds.left()) / step;
        const double fy = (point.y() - bounds.top()) / step;
        const int column = qBound(0, int(std::floor(fx)), columns - 2);
        const int row = qBound(0, int(std::floor(fy)), rows - 2);
        const double u = qBound(0.0, fx - column, 1.0);
        const double v = qBound(0.0, fy - row, 1.0);
        corners[0] = corner(column, row);
        corners[1] = corner(column + 1, row);
        corners[2] = corner(column, row + 1);
        corners[3] = corner(column + 1, row + 1);
        weights[0] = (1 - u) * (1 - v);
        weights[1] = u * (1 - v);
        weights[2] = (1 - u) * v;
        weights[3] = u * v;
    }

    void build(const std::function<bool(const QRect &)> &isStiff);
    void findIslands();
    void factor();
    void pose();
};

void KisPuppet::Private::build(const std::function<bool(const QRect &)> &isStiff)
{
    columns = int(std::ceil(qreal(bounds.width()) / step)) + 1;
    rows = int(std::ceil(qreal(bounds.height()) / step)) + 1;

    rest.resize(columns * rows);
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            rest[corner(column, row)] = QPointF(bounds.left() + column * step, bounds.top() + row * step);
        }
    }
    bent = rest;

    stiff.assign(size_t(columns - 1) * (rows - 1), false);
    for (int row = 0; row < rows - 1; row++) {
        for (int column = 0; column < columns - 1; column++) {
            const QRect cell(bounds.left() + column * step, bounds.top() + row * step, step, step);
            stiff[size_t(row) * (columns - 1) + column] = isStiff(cell & bounds);
        }
    }

    findIslands();

    // every cell as four triangles, both diagonals, so no direction bends more easily
    triangles.clear();
    triangles.reserve(stiff.size() * 4);
    for (int row = 0; row < rows - 1; row++) {
        for (int column = 0; column < columns - 1; column++) {
            const int tl = corner(column, row);
            const int tr = corner(column + 1, row);
            const int bl = corner(column, row + 1);
            const int br = corner(column + 1, row + 1);
            const double weight = 0.5 * (stiff[size_t(row) * (columns - 1) + column] ? 1.0 : airStiffness);
            for (const auto &v : {std::array<int, 3>{tl, tr, br}, std::array<int, 3>{tl, br, bl},
                                  std::array<int, 3>{tl, tr, bl}, std::array<int, 3>{tr, br, bl}}) {
                Triangle t;
                t.weight = weight;
                for (int k = 0; k < 3; k++) {
                    t.v[k] = v[k];
                    const QPointF p0 = rest[v[k]];
                    const QPointF p1 = rest[v[(k + 1) % 3]];
                    const QPointF p2 = rest[v[(k + 2) % 3]];
                    const QPointF e = p1 - p0;
                    const QPointF dd = p2 - p0;
                    const double length = e.x() * e.x() + e.y() * e.y();
                    t.x[k] = (dd.x() * e.x() + dd.y() * e.y()) / length;
                    t.y[k] = (dd.x() * -e.y() + dd.y() * e.x()) / length;
                }
                triangles.push_back(t);
            }
        }
    }
    factored = false;
}

void KisPuppet::Private::findIslands()
{
    const int cellColumns = columns - 1;
    const int cellRows = rows - 1;
    std::vector<int> cellIsland(stiff.size(), -1);
    islands = 0;
    std::vector<int> queue;
    for (int start = 0; start < int(stiff.size()); start++) {
        if (!stiff[size_t(start)] || cellIsland[size_t(start)] >= 0) continue;
        // cells touching even at a corner are one shape
        cellIsland[size_t(start)] = islands;
        queue.assign(1, start);
        for (size_t q = 0; q < queue.size(); q++) {
            const int column = queue[q] % cellColumns;
            const int row = queue[q] / cellColumns;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int c = column + dx;
                    const int r = row + dy;
                    if (c < 0 || r < 0 || c >= cellColumns || r >= cellRows) continue;
                    const int next = r * cellColumns + c;
                    if (!stiff[size_t(next)] || cellIsland[size_t(next)] >= 0) continue;
                    cellIsland[size_t(next)] = islands;
                    queue.push_back(next);
                }
            }
        }
        islands++;
    }

    island.assign(size_t(columns) * rows, -1);
    for (int row = 0; row < cellRows; row++) {
        for (int column = 0; column < cellColumns; column++) {
            const int i = cellIsland[size_t(row) * cellColumns + column];
            if (i < 0) continue;
            for (int c : {corner(column, row), corner(column + 1, row), corner(column, row + 1), corner(column + 1, row + 1)}) {
                island[size_t(c)] = i;
            }
        }
    }

    // the air's nearest island, spreading out from them all at once
    nearest = island;
    queue.clear();
    for (int i = 0; i < int(nearest.size()); i++) {
        if (nearest[size_t(i)] >= 0) queue.push_back(i);
    }
    for (size_t q = 0; q < queue.size(); q++) {
        const int column = queue[q] % columns;
        const int row = queue[q] / columns;
        const int here = nearest[size_t(queue[q])];
        for (const QPoint &offset : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)}) {
            const int c = column + offset.x();
            const int r = row + offset.y();
            if (c < 0 || r < 0 || c >= columns || r >= rows) continue;
            const int next = corner(c, r);
            if (nearest[size_t(next)] >= 0) continue;
            nearest[size_t(next)] = here;
            queue.push_back(next);
        }
    }
}

void KisPuppet::Private::factor()
{
    std::vector<int> pinsOn(size_t(islands), 0);
    for (const Pin &pin : pins) {
        if (pin.island >= 0) pinsOn[size_t(pin.island)]++;
    }
    anchor.assign(rest.size(), airAnchor);
    for (int i = 0; i < rest.size(); i++) {
        const int at = island[size_t(i)];
        if (at < 0) continue;
        anchor[size_t(i)] = pinsOn[size_t(at)] < 2 ? heldAnchor : settle;
    }

    const int n = rest.size();
    std::vector<Eigen::Triplet<double>> scaleFreeTerms;
    std::vector<Eigen::Triplet<double>> fittedTerms;
    scaleFreeTerms.reserve(triangles.size() * 3 * 36 + pins.size() * 32 + 2 * n);
    fittedTerms.reserve(triangles.size() * 12 + pins.size() * 16 + n);

    for (const Triangle &t : triangles) {
        for (int k = 0; k < 3; k++) {
            // the residual of corner 2 against where corners 0 and 1 put it: sum of M_a v_a,
            // with M_2 = I, M_1 = -x I - y R, M_0 = -(1 - x) I + y R and R a quarter turn
            const int v[3] = {t.v[k], t.v[(k + 1) % 3], t.v[(k + 2) % 3]};
            const double x = t.x[k];
            const double y = t.y[k];
            // each M as [a -b; b a]
            const double a[3] = {-(1 - x), -x, 1};
            const double b[3] = {y, -y, 0};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    // M_i^T M_j for M = [a -b; b a]: [ai aj + bi bj, -ai bj + bi aj; ai bj - bi aj, ai aj + bi bj]
                    const double same = t.weight * (a[i] * a[j] + b[i] * b[j]);
                    const double cross = t.weight * (a[i] * b[j] - b[i] * a[j]);
                    scaleFreeTerms.emplace_back(2 * v[i], 2 * v[j], same);
                    scaleFreeTerms.emplace_back(2 * v[i] + 1, 2 * v[j] + 1, same);
                    scaleFreeTerms.emplace_back(2 * v[i], 2 * v[j] + 1, -cross);
                    scaleFreeTerms.emplace_back(2 * v[i] + 1, 2 * v[j], cross);
                }
            }
        }
        for (int k = 0; k < 3; k++) {
            const int i = t.v[k];
            const int j = t.v[(k + 1) % 3];
            fittedTerms.emplace_back(i, i, t.weight);
            fittedTerms.emplace_back(j, j, t.weight);
            fittedTerms.emplace_back(i, j, -t.weight);
            fittedTerms.emplace_back(j, i, -t.weight);
        }
    }
    for (const Pin &pin : pins) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                const double w = pinWeight * pin.weights[i] * pin.weights[j];
                scaleFreeTerms.emplace_back(2 * pin.corners[i], 2 * pin.corners[j], w);
                scaleFreeTerms.emplace_back(2 * pin.corners[i] + 1, 2 * pin.corners[j] + 1, w);
                fittedTerms.emplace_back(pin.corners[i], pin.corners[j], w);
            }
        }
    }
    for (int i = 0; i < n; i++) {
        scaleFreeTerms.emplace_back(2 * i, 2 * i, anchor[size_t(i)]);
        scaleFreeTerms.emplace_back(2 * i + 1, 2 * i + 1, anchor[size_t(i)]);
        fittedTerms.emplace_back(i, i, anchor[size_t(i)]);
    }

    Sparse scaleFreeMatrix(2 * n, 2 * n);
    scaleFreeMatrix.setFromTriplets(scaleFreeTerms.begin(), scaleFreeTerms.end());
    Sparse fittedMatrix(n, n);
    fittedMatrix.setFromTriplets(fittedTerms.begin(), fittedTerms.end());
    scaleFree.compute(scaleFreeMatrix);
    fitted.compute(fittedMatrix);
    factored = true;
}

void KisPuppet::Private::pose()
{
    if (pins.empty()) {
        bent = rest;
        return;
    }
    if (!factored) factor();

    const int n = rest.size();

    // each island's movement by its own pins, turned and moved without scaling: what its corners and air hold to
    std::vector<QVector<QPointF>> from(static_cast<size_t>(islands));
    std::vector<QVector<QPointF>> to(static_cast<size_t>(islands));
    QVector<QPointF> allFrom;
    QVector<QPointF> allTo;
    for (const Pin &pin : pins) {
        if (pin.island >= 0) {
            from[size_t(pin.island)] << pin.rest;
            to[size_t(pin.island)] << pin.at;
        }
        allFrom << pin.rest;
        allTo << pin.at;
    }
    std::vector<Motion> motions(static_cast<size_t>(islands));
    for (int i = 0; i < islands; i++) motions[size_t(i)] = Motion::fit(from[size_t(i)], to[size_t(i)]);
    // with no paint at all, the pins together
    const Motion overall = Motion::fit(allFrom, allTo);
    QVector<QPointF> held(n);
    for (int i = 0; i < n; i++) {
        const int at = nearest[size_t(i)];
        held[i] = at >= 0 ? motions[size_t(at)].map(rest[i]) : overall.map(rest[i]);
    }

    // the pose free to scale
    Eigen::VectorXd b = Eigen::VectorXd::Zero(2 * n);
    for (int i = 0; i < n; i++) {
        b[2 * i] = anchor[size_t(i)] * held[i].x();
        b[2 * i + 1] = anchor[size_t(i)] * held[i].y();
    }
    for (const Pin &pin : pins) {
        for (int k = 0; k < 4; k++) {
            b[2 * pin.corners[k]] += pinWeight * pin.weights[k] * pin.at.x();
            b[2 * pin.corners[k] + 1] += pinWeight * pin.weights[k] * pin.at.y();
        }
    }
    const Eigen::VectorXd scaled = scaleFree.solve(b);

    // each triangle turned and moved, unscaled, to fit that pose; then the corners placed between them
    Eigen::VectorXd bx = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd by = Eigen::VectorXd::Zero(n);
    for (int i = 0; i < n; i++) {
        bx[i] = anchor[size_t(i)] * held[i].x();
        by[i] = anchor[size_t(i)] * held[i].y();
    }
    for (const Triangle &t : triangles) {
        QPointF triangleCenter;
        QPointF posedCenter;
        QPointF posed[3];
        for (int k = 0; k < 3; k++) {
            posed[k] = QPointF(scaled[2 * t.v[k]], scaled[2 * t.v[k] + 1]);
            triangleCenter += rest[t.v[k]];
            posedCenter += posed[k];
        }
        triangleCenter /= 3;
        posedCenter /= 3;
        double along = 0;
        double across = 0;
        QPointF from[3];
        for (int k = 0; k < 3; k++) {
            from[k] = rest[t.v[k]] - triangleCenter;
            const QPointF to = posed[k] - posedCenter;
            along += from[k].x() * to.x() + from[k].y() * to.y();
            across += from[k].x() * to.y() - from[k].y() * to.x();
        }
        const double norm = std::sqrt(along * along + across * across);
        const double c = norm > 0 ? along / norm : 1;
        const double s = norm > 0 ? across / norm : 0;
        QPointF fit[3];
        for (int k = 0; k < 3; k++) {
            fit[k] = posedCenter + QPointF(c * from[k].x() - s * from[k].y(), s * from[k].x() + c * from[k].y());
        }
        for (int k = 0; k < 3; k++) {
            const int i = t.v[k];
            const int j = t.v[(k + 1) % 3];
            const QPointF edge = fit[k] - fit[(k + 1) % 3];
            bx[i] += t.weight * edge.x();
            by[i] += t.weight * edge.y();
            bx[j] -= t.weight * edge.x();
            by[j] -= t.weight * edge.y();
        }
    }
    for (const Pin &pin : pins) {
        for (int k = 0; k < 4; k++) {
            bx[pin.corners[k]] += pinWeight * pin.weights[k] * pin.at.x();
            by[pin.corners[k]] += pinWeight * pin.weights[k] * pin.at.y();
        }
    }
    const Eigen::VectorXd x = fitted.solve(bx);
    const Eigen::VectorXd y = fitted.solve(by);
    for (int i = 0; i < n; i++) bent[i] = QPointF(x[i], y[i]);
}

KisPuppet::KisPuppet(const QRect &bounds, int step, const std::function<bool(const QRect &cell)> &stiff)
    : d(new Private)
{
    d->bounds = bounds;
    d->step = qMax(2, step);
    d->build(stiff);
}

int KisPuppet::stepFor(const QRect &bounds)
{
    // about 6000 cells at most: posing takes a few milliseconds, adding a pin a few tens
    const double step = std::sqrt(double(bounds.width()) * bounds.height() / 6000.0);
    return qMax(8, int(std::ceil(step / 4)) * 4);
}

KisPuppet KisPuppet::ofDevice(KisPaintDeviceSP device, const QRect &bounds)
{
    const int step = stepFor(bounds);
    const int columns = int(std::ceil(qreal(bounds.width()) / step));
    const int rows = int(std::ceil(qreal(bounds.height()) / step));
    std::vector<char> painted(size_t(columns) * rows, 0);

    const KoColorSpace *cs = device->colorSpace();
    const int pixelSize = cs->pixelSize();
    KisGridRenderer::forEachOnAllThreads(rows, [&](int row) {
        const QRect band = QRect(bounds.left(), bounds.top() + row * step, bounds.width(), step) & bounds;
        std::vector<quint8> bytes(size_t(band.width()) * band.height() * pixelSize);
        device->readBytes(bytes.data(), band);
        std::vector<quint8> alpha(size_t(band.width()) * band.height());
        cs->copyOpacityU8(bytes.data(), alpha.data(), int(alpha.size()));
        for (int y = 0; y < band.height(); y++) {
            const quint8 *line = alpha.data() + size_t(y) * band.width();
            for (int x = 0; x < band.width(); x++) {
                if (line[x]) painted[size_t(row) * columns + x / step] = 1;
            }
        }
    });

    return KisPuppet(bounds, step, [&](const QRect &cell) {
        const int column = (cell.left() - bounds.left()) / step;
        const int row = (cell.top() - bounds.top()) / step;
        return column < columns && row < rows && painted[size_t(row) * columns + column];
    });
}

QRect KisPuppet::bounds() const
{
    return d->bounds;
}

int KisPuppet::step() const
{
    return d->step;
}

int KisPuppet::addPin(const QPointF &at)
{
    // the point of the unbent picture now at \p at: the last cell over it in grid order is the one on top
    QPointF rest = at;
    QPolygonF quad(4);
    QPolygonF restQuad(4);
    for (int row = 0; row < d->rows - 1; row++) {
        for (int column = 0; column < d->columns - 1; column++) {
            const int corners[4] = {d->corner(column, row), d->corner(column + 1, row),
                                    d->corner(column + 1, row + 1), d->corner(column, row + 1)};
            for (int k = 0; k < 4; k++) {
                quad[k] = d->bent[corners[k]];
                restQuad[k] = d->rest[corners[k]];
            }
            if (quad.containsPoint(at, Qt::OddEvenFill)) {
                KisFourPointInterpolatorBackward back(restQuad, quad);
                rest = back.map(at);
            }
        }
    }

    Pin pin;
    pin.rest = rest;
    pin.at = at;
    d->cornersOf(rest, pin.corners, pin.weights);
    // the pin holds the shape its point is on, or the nearest one
    const int heaviest = int(std::max_element(pin.weights, pin.weights + 4) - pin.weights);
    pin.island = d->nearest[size_t(pin.corners[heaviest])];
    d->pins.push_back(pin);
    d->factored = false;
    d->pose();
    return int(d->pins.size()) - 1;
}

void KisPuppet::removePin(int index)
{
    if (index < 0 || index >= int(d->pins.size())) return;
    d->pins.erase(d->pins.begin() + index);
    d->factored = false;
    d->pose();
}

void KisPuppet::setPins(const QVector<QPointF> &rest, const QVector<QPointF> &at)
{
    d->pins.clear();
    for (int i = 0; i < qMin(rest.size(), at.size()); i++) {
        Pin pin;
        pin.rest = rest[i];
        pin.at = at[i];
        d->cornersOf(pin.rest, pin.corners, pin.weights);
        const int heaviest = int(std::max_element(pin.weights, pin.weights + 4) - pin.weights);
        pin.island = d->nearest[size_t(pin.corners[heaviest])];
        d->pins.push_back(pin);
    }
    d->factored = false;
    d->pose();
}

void KisPuppet::movePin(int index, const QPointF &to)
{
    if (index < 0 || index >= int(d->pins.size())) return;
    d->pins[size_t(index)].at = to;
    d->pose();
}

int KisPuppet::pinCount() const
{
    return int(d->pins.size());
}

QPointF KisPuppet::pinRest(int index) const
{
    return d->pins[size_t(index)].rest;
}

QPointF KisPuppet::pinAt(int index) const
{
    return d->pins[size_t(index)].at;
}

int KisPuppet::pinNear(const QPointF &point, qreal radius) const
{
    int nearest = -1;
    qreal best = radius * radius;
    for (int i = 0; i < int(d->pins.size()); i++) {
        const QPointF diff = d->pins[size_t(i)].at - point;
        const qreal distance = diff.x() * diff.x() + diff.y() * diff.y();
        if (distance <= best) {
            best = distance;
            nearest = i;
        }
    }
    return nearest;
}

QPointF KisPuppet::map(const QPointF &rest) const
{
    int corners[4];
    double weights[4];
    d->cornersOf(rest, corners, weights);
    QPointF result;
    for (int k = 0; k < 4; k++) result += weights[k] * d->bent[corners[k]];
    // outside the grid the nearest edge's movement carries on
    int nearCorners[4];
    double nearWeights[4];
    const QPointF inside(qBound(qreal(d->bounds.left()), rest.x(), qreal(d->bounds.left() + (d->columns - 1) * d->step)),
                         qBound(qreal(d->bounds.top()), rest.y(), qreal(d->bounds.top() + (d->rows - 1) * d->step)));
    if (inside != rest) {
        d->cornersOf(inside, nearCorners, nearWeights);
        QPointF moved;
        for (int k = 0; k < 4; k++) moved += nearWeights[k] * (d->bent[nearCorners[k]] - d->rest[nearCorners[k]]);
        return rest + moved;
    }
    return result;
}

int KisPuppet::columns() const
{
    return d->columns;
}

int KisPuppet::rows() const
{
    return d->rows;
}

const QVector<QPointF> &KisPuppet::restPoints() const
{
    return d->rest;
}

const QVector<QPointF> &KisPuppet::bentPoints() const
{
    return d->bent;
}

bool KisPuppet::isStiff(int column, int row) const
{
    return d->stiff[size_t(row) * (d->columns - 1) + column];
}
