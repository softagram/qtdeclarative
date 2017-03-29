/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQuick module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qquickpathitemgenericrenderer_p.h"
#include <QtGui/private/qtriangulator_p.h>
#include <QtGui/private/qtriangulatingstroker_p.h>
#include <QThreadPool>

#ifndef QT_NO_OPENGL
#include <QSGVertexColorMaterial>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QtGui/private/qopenglextensions_p.h>
#endif

QT_BEGIN_NAMESPACE

static const qreal TRI_SCALE = 1;

struct ColoredVertex // must match QSGGeometry::ColoredPoint2D
{
    float x, y;
    QQuickPathItemGenericRenderer::Color4ub color;
    void set(float nx, float ny, QQuickPathItemGenericRenderer::Color4ub ncolor)
    {
        x = nx; y = ny; color = ncolor;
    }
};

static inline QQuickPathItemGenericRenderer::Color4ub colorToColor4ub(const QColor &c)
{
    QQuickPathItemGenericRenderer::Color4ub color = {
        uchar(qRound(c.redF() * c.alphaF() * 255)),
        uchar(qRound(c.greenF() * c.alphaF() * 255)),
        uchar(qRound(c.blueF() * c.alphaF() * 255)),
        uchar(qRound(c.alphaF() * 255))
    };
    return color;
}

QQuickPathItemGenericStrokeFillNode::QQuickPathItemGenericStrokeFillNode(QQuickWindow *window)
    : m_geometry(new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0)),
      m_window(window),
      m_material(nullptr)
{
    setGeometry(m_geometry);
    activateMaterial(MatSolidColor);
#ifdef QSG_RUNTIME_DESCRIPTION
    qsgnode_set_description(this, QLatin1String("stroke-fill"));
#endif
}

QQuickPathItemGenericStrokeFillNode::~QQuickPathItemGenericStrokeFillNode()
{
    delete m_geometry;
}

void QQuickPathItemGenericStrokeFillNode::activateMaterial(Material m)
{
    switch (m) {
    case MatSolidColor:
        // Use vertexcolor material. Items with different colors remain batchable
        // this way, at the expense of having to provide per-vertex color values.
        if (!m_solidColorMaterial)
            m_solidColorMaterial.reset(QQuickPathItemGenericMaterialFactory::createVertexColor(m_window));
        m_material = m_solidColorMaterial.data();
        break;
    case MatLinearGradient:
        if (!m_linearGradientMaterial)
            m_linearGradientMaterial.reset(QQuickPathItemGenericMaterialFactory::createLinearGradient(m_window, this));
        m_material = m_linearGradientMaterial.data();
        break;
    default:
        qWarning("Unknown material %d", m);
        return;
    }

    if (material() != m_material)
        setMaterial(m_material);
}

static bool q_supportsElementIndexUint(QSGRendererInterface::GraphicsApi api)
{
    static bool elementIndexUint = true;
#ifndef QT_NO_OPENGL
    if (api == QSGRendererInterface::OpenGL) {
        static bool elementIndexUintChecked = false;
        if (!elementIndexUintChecked) {
            elementIndexUintChecked = true;
            QOpenGLContext *context = QOpenGLContext::currentContext();
            QScopedPointer<QOpenGLContext> dummyContext;
            QScopedPointer<QOffscreenSurface> dummySurface;
            bool ok = true;
            if (!context) {
                dummyContext.reset(new QOpenGLContext);
                dummyContext->create();
                context = dummyContext.data();
                dummySurface.reset(new QOffscreenSurface);
                dummySurface->setFormat(context->format());
                dummySurface->create();
                ok = context->makeCurrent(dummySurface.data());
            }
            if (ok) {
                elementIndexUint = static_cast<QOpenGLExtensions *>(context->functions())->hasOpenGLExtension(
                            QOpenGLExtensions::ElementIndexUint);
            }
        }
    }
#else
    Q_UNUSED(api);
#endif
    return elementIndexUint;
}

QQuickPathItemGenericRenderer::~QQuickPathItemGenericRenderer()
{
    for (VisualPathData &d : m_vp) {
        if (d.pendingFill)
            d.pendingFill->orphaned = true;
        if (d.pendingStroke)
            d.pendingStroke->orphaned = true;
    }
}

// sync, and so triangulation too, happens on the gui thread
//    - except when async is set, in which case triangulation is moved to worker threads

void QQuickPathItemGenericRenderer::beginSync(int totalCount)
{
    if (m_vp.count() != totalCount) {
        m_vp.resize(totalCount);
        m_accDirty |= DirtyList;
    }
    for (VisualPathData &d : m_vp)
        d.syncDirty = 0;
}

void QQuickPathItemGenericRenderer::setPath(int index, const QQuickPath *path)
{
    VisualPathData &d(m_vp[index]);
    d.path = path ? path->path() : QPainterPath();
    d.syncDirty |= DirtyFillGeom | DirtyStrokeGeom;
}

void QQuickPathItemGenericRenderer::setJSPath(int index, const QQuickPathItemPath &path)
{
    VisualPathData &d(m_vp[index]);
    d.path = path.toPainterPath();
    d.syncDirty |= DirtyFillGeom | DirtyStrokeGeom;
}

void QQuickPathItemGenericRenderer::setStrokeColor(int index, const QColor &color)
{
    VisualPathData &d(m_vp[index]);
    d.strokeColor = colorToColor4ub(color);
    d.syncDirty |= DirtyColor;
}

void QQuickPathItemGenericRenderer::setStrokeWidth(int index, qreal w)
{
    VisualPathData &d(m_vp[index]);
    d.strokeWidth = w;
    if (w >= 0.0f)
        d.pen.setWidthF(w);
    d.syncDirty |= DirtyStrokeGeom;
}

void QQuickPathItemGenericRenderer::setFillColor(int index, const QColor &color)
{
    VisualPathData &d(m_vp[index]);
    d.fillColor = colorToColor4ub(color);
    d.syncDirty |= DirtyColor;
}

void QQuickPathItemGenericRenderer::setFillRule(int index, QQuickVisualPath::FillRule fillRule)
{
    VisualPathData &d(m_vp[index]);
    d.fillRule = Qt::FillRule(fillRule);
    d.syncDirty |= DirtyFillGeom;
}

void QQuickPathItemGenericRenderer::setJoinStyle(int index, QQuickVisualPath::JoinStyle joinStyle, int miterLimit)
{
    VisualPathData &d(m_vp[index]);
    d.pen.setJoinStyle(Qt::PenJoinStyle(joinStyle));
    d.pen.setMiterLimit(miterLimit);
    d.syncDirty |= DirtyStrokeGeom;
}

void QQuickPathItemGenericRenderer::setCapStyle(int index, QQuickVisualPath::CapStyle capStyle)
{
    VisualPathData &d(m_vp[index]);
    d.pen.setCapStyle(Qt::PenCapStyle(capStyle));
    d.syncDirty |= DirtyStrokeGeom;
}

void QQuickPathItemGenericRenderer::setStrokeStyle(int index, QQuickVisualPath::StrokeStyle strokeStyle,
                                                   qreal dashOffset, const QVector<qreal> &dashPattern)
{
    VisualPathData &d(m_vp[index]);
    d.pen.setStyle(Qt::PenStyle(strokeStyle));
    if (strokeStyle == QQuickVisualPath::DashLine) {
        d.pen.setDashPattern(dashPattern);
        d.pen.setDashOffset(dashOffset);
    }
    d.syncDirty |= DirtyStrokeGeom;
}

void QQuickPathItemGenericRenderer::setFillGradient(int index, QQuickPathGradient *gradient)
{
    VisualPathData &d(m_vp[index]);
    d.fillGradientActive = gradient != nullptr;
    if (gradient) {
        d.fillGradient.stops = gradient->sortedGradientStops();
        d.fillGradient.spread = gradient->spread();
        if (QQuickPathLinearGradient *g  = qobject_cast<QQuickPathLinearGradient *>(gradient)) {
            d.fillGradient.start = QPointF(g->x1(), g->y1());
            d.fillGradient.end = QPointF(g->x2(), g->y2());
        } else {
            Q_UNREACHABLE();
        }
    }
    d.syncDirty |= DirtyFillGradient;
}

void QQuickPathItemFillRunnable::run()
{
    QQuickPathItemGenericRenderer::triangulateFill(path, fillColor, &fillVertices, &fillIndices, &indexType, supportsElementIndexUint);
    emit done(this);
}

void QQuickPathItemStrokeRunnable::run()
{
    QQuickPathItemGenericRenderer::triangulateStroke(path, pen, strokeColor, &strokeVertices, clipSize);
    emit done(this);
}

void QQuickPathItemGenericRenderer::setAsyncCallback(void (*callback)(void *), void *data)
{
    m_asyncCallback = callback;
    m_asyncCallbackData = data;
}

static QThreadPool *pathWorkThreadPool = nullptr;

static void deletePathWorkThreadPool()
{
    delete pathWorkThreadPool;
    pathWorkThreadPool = nullptr;
}

void QQuickPathItemGenericRenderer::endSync(bool async)
{
    bool didKickOffAsync = false;

    for (int i = 0; i < m_vp.count(); ++i) {
        VisualPathData &d(m_vp[i]);
        if (!d.syncDirty)
            continue;

        m_accDirty |= d.syncDirty;

        // Use a shadow dirty flag in order to avoid losing state in case there are
        // multiple syncs with different dirty flags before we get to updateNode()
        // on the render thread (with the gui thread blocked). For our purposes
        // here syncDirty is still required since geometry regeneration must only
        // happen when there was an actual change in this particular sync round.
        d.effectiveDirty |= d.syncDirty;

        if (d.path.isEmpty()) {
            d.fillVertices.clear();
            d.fillIndices.clear();
            d.strokeVertices.clear();
            continue;
        }

        if (async && !pathWorkThreadPool) {
            qAddPostRoutine(deletePathWorkThreadPool);
            pathWorkThreadPool = new QThreadPool;
            const int idealCount = QThread::idealThreadCount();
            pathWorkThreadPool->setMaxThreadCount(idealCount > 0 ? idealCount * 2 : 4);
        }

        if ((d.syncDirty & DirtyFillGeom) && d.fillColor.a) {
            d.path.setFillRule(d.fillRule);
            if (m_api == QSGRendererInterface::Unknown)
                m_api = m_item->window()->rendererInterface()->graphicsApi();
            if (async) {
                QQuickPathItemFillRunnable *r = new QQuickPathItemFillRunnable;
                r->setAutoDelete(false);
                if (d.pendingFill)
                    d.pendingFill->orphaned = true;
                d.pendingFill = r;
                r->path = d.path;
                r->fillColor = d.fillColor;
                r->supportsElementIndexUint = q_supportsElementIndexUint(m_api);
                // Unlikely in practice but in theory m_vp could be
                // resized. Therefore, capture 'i' instead of 'd'.
                QObject::connect(r, &QQuickPathItemFillRunnable::done, qApp, [this, i](QQuickPathItemFillRunnable *r) {
                    // Bail out when orphaned (meaning either another run was
                    // started after this one, or the renderer got destroyed).
                    if (!r->orphaned && i < m_vp.count()) {
                        VisualPathData &d(m_vp[i]);
                        d.fillVertices = r->fillVertices;
                        d.fillIndices = r->fillIndices;
                        d.indexType = r->indexType;
                        d.pendingFill = nullptr;
                        d.effectiveDirty |= DirtyFillGeom;
                        maybeUpdateAsyncItem();
                    }
                    r->deleteLater();
                });
                didKickOffAsync = true;
                pathWorkThreadPool->start(r);
            } else {
                triangulateFill(d.path, d.fillColor, &d.fillVertices, &d.fillIndices, &d.indexType, q_supportsElementIndexUint(m_api));
            }
        }

        if ((d.syncDirty & DirtyStrokeGeom) && d.strokeWidth >= 0.0f && d.strokeColor.a) {
            if (async) {
                QQuickPathItemStrokeRunnable *r = new QQuickPathItemStrokeRunnable;
                r->setAutoDelete(false);
                if (d.pendingStroke)
                    d.pendingStroke->orphaned = true;
                d.pendingStroke = r;
                r->path = d.path;
                r->pen = d.pen;
                r->strokeColor = d.strokeColor;
                r->clipSize = QSize(m_item->width(), m_item->height());
                QObject::connect(r, &QQuickPathItemStrokeRunnable::done, qApp, [this, i](QQuickPathItemStrokeRunnable *r) {
                    if (!r->orphaned && i < m_vp.count()) {
                        VisualPathData &d(m_vp[i]);
                        d.strokeVertices = r->strokeVertices;
                        d.pendingStroke = nullptr;
                        d.effectiveDirty |= DirtyStrokeGeom;
                        maybeUpdateAsyncItem();
                    }
                    r->deleteLater();
                });
                didKickOffAsync = true;
                pathWorkThreadPool->start(r);
            } else {
                triangulateStroke(d.path, d.pen, d.strokeColor, &d.strokeVertices,
                                  QSize(m_item->width(), m_item->height()));
            }
        }
    }

    if (!didKickOffAsync && async && m_asyncCallback)
        m_asyncCallback(m_asyncCallbackData);
}

void QQuickPathItemGenericRenderer::maybeUpdateAsyncItem()
{
    for (const VisualPathData &d : qAsConst(m_vp)) {
        if (d.pendingFill || d.pendingStroke)
            return;
    }
    m_accDirty |= DirtyFillGeom | DirtyStrokeGeom;
    m_item->update();
    if (m_asyncCallback)
        m_asyncCallback(m_asyncCallbackData);
}

// the stroke/fill triangulation functions may be invoked either on the gui
// thread or some worker thread and must thus be self-contained.
void QQuickPathItemGenericRenderer::triangulateFill(const QPainterPath &path,
                                                    const Color4ub &fillColor,
                                                    VertexContainerType *fillVertices,
                                                    IndexContainerType *fillIndices,
                                                    QSGGeometry::Type *indexType,
                                                    bool supportsElementIndexUint)
{
    const QVectorPath &vp = qtVectorPathForPath(path);

    QTriangleSet ts = qTriangulate(vp, QTransform::fromScale(TRI_SCALE, TRI_SCALE), 1, supportsElementIndexUint);
    const int vertexCount = ts.vertices.count() / 2; // just a qreal vector with x,y hence the / 2
    fillVertices->resize(vertexCount);
    ColoredVertex *vdst = reinterpret_cast<ColoredVertex *>(fillVertices->data());
    const qreal *vsrc = ts.vertices.constData();
    for (int i = 0; i < vertexCount; ++i)
        vdst[i].set(vsrc[i * 2] / TRI_SCALE, vsrc[i * 2 + 1] / TRI_SCALE, fillColor);

    size_t indexByteSize;
    if (ts.indices.type() == QVertexIndexVector::UnsignedShort) {
        *indexType = QSGGeometry::UnsignedShortType;
        // fillIndices is still QVector<quint32>. Just resize to N/2 and pack
        // the N quint16s into it.
        fillIndices->resize(ts.indices.size() / 2);
        indexByteSize = ts.indices.size() * sizeof(quint16);
    } else {
        *indexType = QSGGeometry::UnsignedIntType;
        fillIndices->resize(ts.indices.size());
        indexByteSize = ts.indices.size() * sizeof(quint32);
    }
    memcpy(fillIndices->data(), ts.indices.data(), indexByteSize);
}

void QQuickPathItemGenericRenderer::triangulateStroke(const QPainterPath &path,
                                                      const QPen &pen,
                                                      const Color4ub &strokeColor,
                                                      VertexContainerType *strokeVertices,
                                                      const QSize &clipSize)
{
    const QVectorPath &vp = qtVectorPathForPath(path);
    const QRectF clip(QPointF(0, 0), clipSize);
    const qreal inverseScale = 1.0 / TRI_SCALE;

    QTriangulatingStroker stroker;
    stroker.setInvScale(inverseScale);

    if (pen.style() == Qt::SolidLine) {
        stroker.process(vp, pen, clip, 0);
    } else {
        QDashedStrokeProcessor dashStroker;
        dashStroker.setInvScale(inverseScale);
        dashStroker.process(vp, pen, clip, 0);
        QVectorPath dashStroke(dashStroker.points(), dashStroker.elementCount(),
                               dashStroker.elementTypes(), 0);
        stroker.process(dashStroke, pen, clip, 0);
    }

    if (!stroker.vertexCount()) {
        strokeVertices->clear();
        return;
    }

    const int vertexCount = stroker.vertexCount() / 2; // just a float vector with x,y hence the / 2
    strokeVertices->resize(vertexCount);
    ColoredVertex *vdst = reinterpret_cast<ColoredVertex *>(strokeVertices->data());
    const float *vsrc = stroker.vertices();
    for (int i = 0; i < vertexCount; ++i)
        vdst[i].set(vsrc[i * 2], vsrc[i * 2 + 1], strokeColor);
}

void QQuickPathItemGenericRenderer::setRootNode(QQuickPathItemGenericNode *node)
{
    if (m_rootNode != node) {
        m_rootNode = node;
        m_accDirty |= DirtyList;
    }
}

// on the render thread with gui blocked
void QQuickPathItemGenericRenderer::updateNode()
{
    if (!m_rootNode || !m_accDirty)
        return;

//                     [   m_rootNode   ]
//                     /       /        /
// #0          [  fill  ] [  stroke  ] [   next   ]
//                                    /     /      |
// #1                       [  fill  ] [  stroke  ] [   next   ]
//                                                 /      /     |
// #2                                     [  fill  ] [ stroke ] [  next  ]
//                                                                 ...
// ...

    QQuickPathItemGenericNode **nodePtr = &m_rootNode;
    QQuickPathItemGenericNode *prevNode = nullptr;

    for (VisualPathData &d : m_vp) {
        if (!*nodePtr) {
            *nodePtr = new QQuickPathItemGenericNode;
            prevNode->m_next = *nodePtr;
            prevNode->appendChildNode(*nodePtr);
        }

        QQuickPathItemGenericNode *node = *nodePtr;

        if (m_accDirty & DirtyList)
            d.effectiveDirty |= DirtyFillGeom | DirtyStrokeGeom | DirtyColor | DirtyFillGradient;

        if (!d.effectiveDirty) {
            prevNode = node;
            nodePtr = &node->m_next;
            continue;
        }

        if (d.fillColor.a == 0) {
            delete node->m_fillNode;
            node->m_fillNode = nullptr;
        } else if (!node->m_fillNode) {
            node->m_fillNode = new QQuickPathItemGenericStrokeFillNode(m_item->window());
            if (node->m_strokeNode)
                node->removeChildNode(node->m_strokeNode);
            node->appendChildNode(node->m_fillNode);
            if (node->m_strokeNode)
                node->appendChildNode(node->m_strokeNode);
            d.effectiveDirty |= DirtyFillGeom;
        }

        if (d.strokeWidth < 0.0f || d.strokeColor.a == 0) {
            delete node->m_strokeNode;
            node->m_strokeNode = nullptr;
        } else if (!node->m_strokeNode) {
            node->m_strokeNode = new QQuickPathItemGenericStrokeFillNode(m_item->window());
            node->appendChildNode(node->m_strokeNode);
            d.effectiveDirty |= DirtyStrokeGeom;
        }

        updateFillNode(&d, node);
        updateStrokeNode(&d, node);

        d.effectiveDirty = 0;

        prevNode = node;
        nodePtr = &node->m_next;
    }

    if (*nodePtr && prevNode) {
        prevNode->removeChildNode(*nodePtr);
        delete *nodePtr;
        *nodePtr = nullptr;
    }

    m_accDirty = 0;
}

void QQuickPathItemGenericRenderer::updateShadowDataInNode(VisualPathData *d, QQuickPathItemGenericStrokeFillNode *n)
{
    if (d->fillGradientActive) {
        if (d->effectiveDirty & DirtyFillGradient)
            n->m_fillGradient = d->fillGradient;
    }
}

void QQuickPathItemGenericRenderer::updateFillNode(VisualPathData *d, QQuickPathItemGenericNode *node)
{
    if (!node->m_fillNode)
        return;
    if (!(d->effectiveDirty & (DirtyFillGeom | DirtyColor | DirtyFillGradient)))
        return;

    // Make a copy of the data that will be accessed by the material on
    // the render thread. This must be done even when we bail out below.
    QQuickPathItemGenericStrokeFillNode *n = node->m_fillNode;
    updateShadowDataInNode(d, n);

    QSGGeometry *g = n->m_geometry;
    if (d->fillVertices.isEmpty()) {
        if (g->vertexCount() || g->indexCount()) {
            g->allocate(0, 0);
            n->markDirty(QSGNode::DirtyGeometry);
        }
        return;
    }

    if (d->fillGradientActive) {
        n->activateMaterial(QQuickPathItemGenericStrokeFillNode::MatLinearGradient);
        if (d->effectiveDirty & DirtyFillGradient) {
            // Gradients are implemented via a texture-based material.
            n->markDirty(QSGNode::DirtyMaterial);
            // stop here if only the gradient changed; no need to touch the geometry
            if (!(d->effectiveDirty & DirtyFillGeom))
                return;
        }
    } else {
        n->activateMaterial(QQuickPathItemGenericStrokeFillNode::MatSolidColor);
        // fast path for updating only color values when no change in vertex positions
        if ((d->effectiveDirty & DirtyColor) && !(d->effectiveDirty & DirtyFillGeom)) {
            ColoredVertex *vdst = reinterpret_cast<ColoredVertex *>(g->vertexData());
            for (int i = 0; i < g->vertexCount(); ++i)
                vdst[i].set(vdst[i].x, vdst[i].y, d->fillColor);
            n->markDirty(QSGNode::DirtyGeometry);
            return;
        }
    }

    const int indexCount = d->indexType == QSGGeometry::UnsignedShortType
            ? d->fillIndices.count() * 2 : d->fillIndices.count();
    if (g->indexType() != d->indexType) {
        g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                            d->fillVertices.count(), indexCount, d->indexType);
        n->setGeometry(g);
        delete n->m_geometry;
        n->m_geometry = g;
    } else {
        g->allocate(d->fillVertices.count(), indexCount);
    }
    g->setDrawingMode(QSGGeometry::DrawTriangles);
    memcpy(g->vertexData(), d->fillVertices.constData(), g->vertexCount() * g->sizeOfVertex());
    memcpy(g->indexData(), d->fillIndices.constData(), g->indexCount() * g->sizeOfIndex());

    n->markDirty(QSGNode::DirtyGeometry);
}

void QQuickPathItemGenericRenderer::updateStrokeNode(VisualPathData *d, QQuickPathItemGenericNode *node)
{
    if (!node->m_strokeNode)
        return;
    if (!(d->effectiveDirty & (DirtyStrokeGeom | DirtyColor)))
        return;

    QQuickPathItemGenericStrokeFillNode *n = node->m_strokeNode;
    QSGGeometry *g = n->m_geometry;
    if (d->strokeVertices.isEmpty()) {
        if (g->vertexCount() || g->indexCount()) {
            g->allocate(0, 0);
            n->markDirty(QSGNode::DirtyGeometry);
        }
        return;
    }

    n->markDirty(QSGNode::DirtyGeometry);

    // Async loading runs update once, bails out above, then updates again once
    // ready. Set the material dirty then. This is in-line with fill where the
    // first activateMaterial() achieves the same.
    if (!g->vertexCount())
        n->markDirty(QSGNode::DirtyMaterial);

    if ((d->effectiveDirty & DirtyColor) && !(d->effectiveDirty & DirtyStrokeGeom)) {
        ColoredVertex *vdst = reinterpret_cast<ColoredVertex *>(g->vertexData());
        for (int i = 0; i < g->vertexCount(); ++i)
            vdst[i].set(vdst[i].x, vdst[i].y, d->strokeColor);
        return;
    }

    g->allocate(d->strokeVertices.count(), 0);
    g->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    memcpy(g->vertexData(), d->strokeVertices.constData(), g->vertexCount() * g->sizeOfVertex());
}

QSGMaterial *QQuickPathItemGenericMaterialFactory::createVertexColor(QQuickWindow *window)
{
    QSGRendererInterface::GraphicsApi api = window->rendererInterface()->graphicsApi();

#ifndef QT_NO_OPENGL
    if (api == QSGRendererInterface::OpenGL) // ### so much for "generic"...
        return new QSGVertexColorMaterial;
#endif

    qWarning("Vertex-color material: Unsupported graphics API %d", api);
    return nullptr;
}

QSGMaterial *QQuickPathItemGenericMaterialFactory::createLinearGradient(QQuickWindow *window,
                                                                        QQuickPathItemGenericStrokeFillNode *node)
{
    QSGRendererInterface::GraphicsApi api = window->rendererInterface()->graphicsApi();

#ifndef QT_NO_OPENGL
    if (api == QSGRendererInterface::OpenGL) // ### so much for "generic"...
        return new QQuickPathItemLinearGradientMaterial(node);
#endif

    qWarning("Linear gradient material: Unsupported graphics API %d", api);
    return nullptr;
}

#ifndef QT_NO_OPENGL

QSGMaterialType QQuickPathItemLinearGradientShader::type;

QQuickPathItemLinearGradientShader::QQuickPathItemLinearGradientShader()
{
    setShaderSourceFile(QOpenGLShader::Vertex,
                        QStringLiteral(":/qt-project.org/items/shaders/lineargradient.vert"));
    setShaderSourceFile(QOpenGLShader::Fragment,
                        QStringLiteral(":/qt-project.org/items/shaders/lineargradient.frag"));
}

void QQuickPathItemLinearGradientShader::initialize()
{
    m_opacityLoc = program()->uniformLocation("opacity");
    m_matrixLoc = program()->uniformLocation("matrix");
    m_gradStartLoc = program()->uniformLocation("gradStart");
    m_gradEndLoc = program()->uniformLocation("gradEnd");
}

void QQuickPathItemLinearGradientShader::updateState(const RenderState &state, QSGMaterial *mat, QSGMaterial *)
{
    QQuickPathItemLinearGradientMaterial *m = static_cast<QQuickPathItemLinearGradientMaterial *>(mat);

    if (state.isOpacityDirty())
        program()->setUniformValue(m_opacityLoc, state.opacity());

    if (state.isMatrixDirty())
        program()->setUniformValue(m_matrixLoc, state.combinedMatrix());

    QQuickPathItemGenericStrokeFillNode *node = m->node();
    program()->setUniformValue(m_gradStartLoc, QVector2D(node->m_fillGradient.start));
    program()->setUniformValue(m_gradEndLoc, QVector2D(node->m_fillGradient.end));

    QSGTexture *tx = QQuickPathItemGradientCache::currentCache()->get(node->m_fillGradient);
    tx->bind();
}

char const *const *QQuickPathItemLinearGradientShader::attributeNames() const
{
    static const char *const attr[] = { "vertexCoord", "vertexColor", nullptr };
    return attr;
}

int QQuickPathItemLinearGradientMaterial::compare(const QSGMaterial *other) const
{
    Q_ASSERT(other && type() == other->type());
    const QQuickPathItemLinearGradientMaterial *m = static_cast<const QQuickPathItemLinearGradientMaterial *>(other);

    QQuickPathItemGenericStrokeFillNode *a = node();
    QQuickPathItemGenericStrokeFillNode *b = m->node();
    Q_ASSERT(a && b);
    if (a == b)
        return 0;

    const QQuickPathItemGradientCache::GradientDesc *ga = &a->m_fillGradient;
    const QQuickPathItemGradientCache::GradientDesc *gb = &b->m_fillGradient;

    if (int d = ga->spread - gb->spread)
        return d;

    if (int d = ga->start.x() - gb->start.x())
        return d;
    if (int d = ga->start.y() - gb->start.y())
        return d;
    if (int d = ga->end.x() - gb->end.x())
        return d;
    if (int d = ga->end.y() - gb->end.y())
        return d;

    if (int d = ga->stops.count() - gb->stops.count())
        return d;

    for (int i = 0; i < ga->stops.count(); ++i) {
        if (int d = ga->stops[i].first - gb->stops[i].first)
            return d;
        if (int d = ga->stops[i].second.rgba() - gb->stops[i].second.rgba())
            return d;
    }

    return 0;
}

#endif // QT_NO_OPENGL

QT_END_NAMESPACE