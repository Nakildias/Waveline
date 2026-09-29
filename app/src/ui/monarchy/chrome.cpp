// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "chrome.h"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QRadialGradient>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWindow>

#include "desktop.h"
#include "../theme.h"

#include <algorithm>

#ifdef WAVELINE_HAVE_KWINDOWSYSTEM
#include <KWindowEffects>
#include <KWindowShadow>

#include "boxshadowrenderer.h"
#endif

namespace Monarchy {

namespace {

// ============================================================ traffic lights
// Everything here is Monarchy's common/windowchrome/window_buttons.cc and
// common/titlebar/traffic_light_glass.h, which in turn are the decoration's
// src/shell/titlebar/breezebutton.cpp: an 18-unit design drawn in a 20-unit
// box, a dot of radius 7 that grows to 9 under the pointer.

using Style = Settings::ButtonStyle;

constexpr qreal kDesignBox = 20.0;
constexpr qreal kCentre = 9.0;
constexpr qreal kRadiusIdle = 7.0;
constexpr qreal kRadiusHover = 9.0;
constexpr qreal kButtonPen = 1.01;
constexpr qreal kSymbolPen = 1.7;
const QColor kDarkSymbol(34, 45, 50);
const QColor kLightSymbol(250, 251, 252);

enum class Light { Close, Minimise, Maximise };

bool isSbeSierra(Style s) {
    return s == Style::SbeSierra || s == Style::SbeSierraActive
        || s == Style::SbeSierraInactive;
}

QColor lightColour(Light light) {
    switch (light) {
    case Light::Close:    return QColor(255, 59, 48);
    case Light::Minimise: return QColor(255, 204, 0);
    case Light::Maximise: return QColor(40, 215, 60);
    }
    return QColor();
}

QColor symbolColour(const QColor &bar, bool inactive, bool useActive, bool useInactive) {
    if (useActive || (!inactive && !useInactive)) return kDarkSymbol;
    const qreal lum = 0.299 * qRed(bar.rgb()) + 0.587 * qGreen(bar.rgb())
                    + 0.114 * qBlue(bar.rgb());
    return (lum > 186 || qGreen(bar.rgb()) > 186) ? kDarkSymbol : kLightSymbol;
}

// QColor::lighter()/darker() move the value alone, which drains a bright hue;
// this pushes the saturation the other way so the light end stays red.
QColor shade(const QColor &c, qreal valueFactor, qreal saturationFactor) {
    float h = 0, s = 0, v = 0, a = 0;
    c.toHsv().getHsvF(&h, &s, &v, &a);
    return QColor::fromHsvF(h, qBound(0.0F, float(s * saturationFactor), 1.0F),
                            qBound(0.0F, float(v * valueFactor), 1.0F), a);
}

// The glass bead: contact shadow, domed body, rim light off the far side, the
// edge turning away, and the sheen across the top.
void paintGlass(QPainter *p, const QPointF &centre, qreal radius, const QColor &colour) {
    const QRectF disc(centre.x() - radius, centre.y() - radius, radius * 2, radius * 2);
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(Qt::NoPen);

    const QPointF contactCentre = centre + QPointF(0, radius * 0.22);
    QRadialGradient contact(contactCentre, radius * 1.28);
    contact.setColorAt(0.0, QColor(0, 0, 0, 52));
    contact.setColorAt(0.70, QColor(0, 0, 0, 38));
    contact.setColorAt(1.0, QColor(0, 0, 0, 0));
    p->setBrush(contact);
    p->drawEllipse(contactCentre, radius * 1.28, radius * 1.28);

    QLinearGradient body(disc.left(), disc.top(), disc.left(), disc.bottom());
    body.setColorAt(0.0, shade(colour, 1.10, 0.96));
    body.setColorAt(0.46, shade(colour, 1.0, 1.08));
    body.setColorAt(1.0, shade(colour, 0.84, 1.14));
    p->setBrush(body);
    p->drawEllipse(disc);

    QPainterPath bead;
    bead.addEllipse(disc);
    p->setClipPath(bead, Qt::IntersectClip);

    QRadialGradient rim(centre + QPointF(0, radius * 0.95), radius * 1.05);
    rim.setColorAt(0.0, QColor(255, 255, 255, 140));
    rim.setColorAt(0.48, QColor(255, 255, 255, 42));
    rim.setColorAt(1.0, QColor(255, 255, 255, 0));
    p->setBrush(rim);
    p->drawEllipse(disc);

    QRadialGradient edge(centre, radius);
    edge.setColorAt(0.0, QColor(0, 0, 0, 0));
    edge.setColorAt(0.74, QColor(0, 0, 0, 0));
    edge.setColorAt(1.0, QColor(0, 0, 0, 58));
    p->setBrush(edge);
    p->drawEllipse(disc);

    const QRectF gloss(centre.x() - radius * 0.64, centre.y() - radius * 0.88,
                       radius * 1.28, radius * 0.92);
    QLinearGradient sheen(gloss.left(), gloss.top(), gloss.left(), gloss.bottom());
    sheen.setColorAt(0.0, QColor(255, 255, 255, 112));
    sheen.setColorAt(0.65, QColor(255, 255, 255, 26));
    sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
    p->setBrush(sheen);
    p->drawEllipse(gloss);

    p->restore();
}

class TrafficLight : public QAbstractButton {
public:
    TrafficLight(Light light, QWidget *parent) : QAbstractButton(parent), light_(light) {
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        hover_.setStartValue(0.0);
        hover_.setEndValue(1.0);
        hover_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&hover_, &QVariantAnimation::valueChanged, this,
                         [this] { update(); });
        applySettings();
    }

    void applySettings() {
        const Settings &s = Settings::instance();
        setFixedSize(s.buttonSize(), s.buttonSize());
        hover_.setDuration(s.animationsDuration());
        if (!s.animationsEnabled()) hover_.stop();
        update();
    }

protected:
    void enterEvent(QEnterEvent *) override { animate(true); }
    void leaveEvent(QEvent *) override { animate(false); }

    void paintEvent(QPaintEvent *) override {
        const Settings &s = Settings::instance();
        const bool animated = s.animationsEnabled();
        const bool sbe = isSbeSierra(s.buttonStyle());
        const bool useActive = s.buttonStyle() == Style::SbeSierraActive;
        const bool useInactive = s.buttonStyle() == Style::SbeSierraInactive;
        const bool inactive = !window() || !window()->isActiveWindow();
        const bool wearsInactive = sbe && ((inactive && !useActive) || useInactive);

        const QColor bar = palette().color(QPalette::Window);
        const bool darkBar = qGray(bar.rgb()) < 128;

        QColor colour;
        if (sbe || !inactive)
            colour = lightColour(light_);
        else
            colour = darkBar ? QColor(100, 100, 100) : QColor(200, 200, 200);

        const qreal box = width();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // With animations off the decoration draws the design seven ninths as
        // large and centred differently; so does this.
        if (animated) {
            p.scale(box / kDesignBox, box / kDesignBox);
            p.translate(1, 1);
        } else {
            p.scale(7.0 / 9.0 * box / kDesignBox, 7.0 / 9.0 * box / kDesignBox);
            p.translate(4, 4);
        }
        const qreal penScale = qMax(qreal(1.0), kDesignBox / box);
        const qreal thicken = animated ? 1.0 : 9.0 / 7.0;

        const qreal grown = hover_.currentValue().isValid()
                                ? hover_.currentValue().toReal() : 0.0;
        const qreal radius = animated
            ? kRadiusIdle + (kRadiusHover - kRadiusIdle) * grown : kRadiusHover;

        QPen outline(qGray(bar.rgb()) < 69 ? colour.lighter(115) : colour.darker(115));
        outline.setJoinStyle(Qt::MiterJoin);
        outline.setWidthF(thicken * kButtonPen * penScale);

        if (wearsInactive && underMouse()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(outline);
        } else if (wearsInactive) {
            p.setBrush(Qt::NoBrush);
            p.setPen(Qt::NoPen);
        } else {
            paintGlass(&p, QPointF(kCentre, kCentre), radius, colour);
            p.setBrush(Qt::NoBrush);
            p.setPen(outline);
        }
        p.drawEllipse(QPointF(kCentre, kCentre), radius, radius);

        const bool showSymbol = underMouse() || wearsInactive;
        if (!showSymbol) return;
        if (!sbe && inactive) return;

        const QColor ink = symbolColour(bar, inactive, useActive, useInactive);
        QPen symbol(ink);
        symbol.setJoinStyle(Qt::MiterJoin);
        symbol.setWidthF(thicken * kSymbolPen * penScale);
        p.setPen(symbol);

        switch (light_) {
        case Light::Close:
            p.drawLine(QPointF(6, 6), QPointF(12, 12));
            p.drawLine(QPointF(6, 12), QPointF(12, 6));
            break;
        case Light::Minimise:
            p.drawLine(QPointF(5, 9), QPointF(13, 9));
            break;
        case Light::Maximise: {
            QPainterPath first, second;
            if (window() && window()->isMaximized()) {
                first.moveTo(8.5, 9.5);  first.lineTo(2.5, 9.5);  first.lineTo(8.5, 15.5);
                second.moveTo(9.5, 8.5); second.lineTo(15.5, 8.5); second.lineTo(9.5, 2.5);
            } else {
                first.moveTo(5, 13);  first.lineTo(11, 13); first.lineTo(5, 7);
                second.moveTo(13, 5); second.lineTo(7, 5);  second.lineTo(13, 11);
            }
            p.fillPath(first, ink);
            p.fillPath(second, ink);
            break;
        }
        }
    }

private:
    void animate(bool entering) {
        if (!Settings::instance().animationsEnabled()) {
            update();
            return;
        }
        hover_.stop();
        hover_.setDirection(entering ? QAbstractAnimation::Forward
                                     : QAbstractAnimation::Backward);
        hover_.start();
        update();
    }

    Light light_;
    QVariantAnimation hover_;
};

// ===================================================================== shadow
#ifdef WAVELINE_HAVE_KWINDOWSYSTEM
// The decoration's shadow, for a window without one: Monarchy's
// common/windowchrome/window_shadow.cc, which is Breeze's
// Decoration::updateActiveShadow() handed to KWin as the window's own.

constexpr int kShadowOverlap = 3;

struct ShadowLayer {
    QPoint offset;
    int radius;
    qreal opacity;
};
struct ShadowParams {
    QPoint offset;
    ShadowLayer first;
    ShadowLayer second;
};
const ShadowParams kShadowParams[] = {
    {{0, 0}, {{0, 0}, 0, 0}, {{0, 0}, 0, 0}},
    {{0, 4}, {{0, 0}, 16, 1.0}, {{0, -2}, 8, 0.4}},
    {{0, 8}, {{0, 0}, 32, 0.9}, {{0, -4}, 16, 0.3}},
    {{0, 12}, {{0, 0}, 48, 0.8}, {{0, -6}, 24, 0.2}},
    {{0, 16}, {{0, 0}, 64, 0.7}, {{0, -8}, 32, 0.1}},
};

class ShadowAttachment : public QObject {
public:
    explicit ShadowAttachment(QWidget *window)
        : QObject(window), window_(window), shadow_(new KWindowShadow(this)) {
        window->installEventFilter(this);
        QObject::connect(&Settings::instance(), &Settings::changed, this,
                         [this] { render(); rebuild(); });
        render();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == window_) {
            switch (event->type()) {
            case QEvent::Show:
                attachHandle();
                rebuild();
                break;
            case QEvent::Hide:
                shadow_->destroy();
                break;
            case QEvent::WindowStateChange:
                rebuild();
                break;
            default:
                break;
            }
        } else if (watched == handle_) {
            if (event->type() == QEvent::Expose && handle_->isExposed()
                && !shadow_->isCreated())
                rebuild();
            else if (event->type() == QEvent::PlatformSurface
                     && static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
                            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
                shadow_->destroy();
        }
        return false;
    }

private:
    void attachHandle() {
        QWindow *handle = window_->windowHandle();
        if (!handle || handle == handle_) return;
        if (handle_) handle_->removeEventFilter(this);
        handle_ = handle;
        handle_->installEventFilter(this);
    }

    void render() {
        const Settings &s = Settings::instance();
        const ShadowParams &params = kShadowParams[qBound(0, s.shadowSize(), 4)];
        none_ = qMax(params.first.radius, params.second.radius) == 0;
        if (none_) return;

        const auto withOpacity = [](QColor c, qreal o) { c.setAlphaF(o); return c; };
        const int spacing = Settings::smallSpacing();
        const qreal radius = 0.5 * spacing * (s.cornerRadius() + 0.5);
        const QSize boxSize =
            Breeze::BoxShadowRenderer::calculateMinimumBoxSize(2 * spacing * params.first.radius)
                .expandedTo(Breeze::BoxShadowRenderer::calculateMinimumBoxSize(
                    2 * spacing * params.second.radius));

        Breeze::BoxShadowRenderer renderer;
        renderer.setBorderRadius(radius);
        renderer.setBoxSize(boxSize);
        const qreal factor = s.shadowStrength() / 255.0;
        renderer.addShadow(params.first.offset, params.first.radius,
                           withOpacity(s.shadowColor(), params.first.opacity * factor));
        renderer.addShadow(params.second.offset, params.second.radius,
                           withOpacity(s.shadowColor(), params.second.opacity * factor));

        QImage image = renderer.render();
        const QRect outer = image.rect();
        QRect box(QPoint(0, 0), boxSize);
        box.moveCenter(outer.center());
        padding_ = QMargins(box.left() - outer.left() - kShadowOverlap - params.offset.x(),
                            box.top() - outer.top() - kShadowOverlap - params.offset.y(),
                            outer.right() - box.right() - kShadowOverlap + params.offset.x(),
                            outer.bottom() - box.bottom() - kShadowOverlap + params.offset.y());
        {
            // The window's own shape cut out, so nothing of the shadow shows
            // through a translucent window.
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawRoundedRect(outer - padding_, radius, radius);
        }

        const int cx = outer.center().x();
        const int cy = outer.center().y();
        const int right = image.width() - cx - 1;
        const int bottom = image.height() - cy - 1;
        const QRect pieces[8] = {
            {0, 0, cx, cy},                  {cx, 0, 1, cy},
            {cx + 1, 0, right, cy},          {cx + 1, cy, right, 1},
            {cx + 1, cy + 1, right, bottom}, {cx, cy + 1, 1, bottom},
            {0, cy + 1, cx, bottom},         {0, cy, cx, 1},
        };
        for (int i = 0; i < 8; ++i) {
            tiles_[i] = KWindowShadowTile::Ptr::create();
            tiles_[i]->setImage(image.copy(pieces[i]));
        }
    }

    // A created shadow is fixed, so any change is a destroy and a create.
    void rebuild() {
        shadow_->destroy();
        if (!handle_ || !window_->isVisible() || none_) return;
        if (window_->windowState() & Qt::WindowFullScreen) return;
        shadow_->setTopLeftTile(tiles_[0]);
        shadow_->setTopTile(tiles_[1]);
        shadow_->setTopRightTile(tiles_[2]);
        shadow_->setRightTile(tiles_[3]);
        shadow_->setBottomRightTile(tiles_[4]);
        shadow_->setBottomTile(tiles_[5]);
        shadow_->setBottomLeftTile(tiles_[6]);
        shadow_->setLeftTile(tiles_[7]);
        shadow_->setPadding(padding_);
        shadow_->setWindow(handle_);
        shadow_->create();
    }

    QWidget *window_;
    QPointer<QWindow> handle_;
    KWindowShadow *shadow_;
    KWindowShadowTile::Ptr tiles_[8];
    QMargins padding_;
    bool none_ = true;
};

// KWin's blur behind the translucent background -- the frosted look every
// Monarchy window has. Re-sent on every show: a new surface forgets it.
class BlurAttachment : public QObject {
public:
    explicit BlurAttachment(QWidget *window) : QObject(window), window_(window) {
        window->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == window_ && event->type() == QEvent::Show)
            if (QWindow *h = window_->windowHandle())
                KWindowEffects::enableBlurBehind(h, true);
        return false;
    }

private:
    QWidget *window_;
};
#endif  // WAVELINE_HAVE_KWINDOWSYSTEM

Qt::Edges resizeEdgesAt(const QWidget *central, const QPoint &pos) {
    Qt::Edges edges;
    if (pos.x() < kResizeMargin) edges |= Qt::LeftEdge;
    if (pos.x() >= central->width() - kResizeMargin) edges |= Qt::RightEdge;
    if (pos.y() < kResizeMargin) edges |= Qt::TopEdge;
    if (pos.y() >= central->height() - kResizeMargin) edges |= Qt::BottomEdge;
    return edges;
}

Qt::CursorShape cursorForEdges(Qt::Edges e) {
    if (((e & Qt::LeftEdge) && (e & Qt::TopEdge))
        || ((e & Qt::RightEdge) && (e & Qt::BottomEdge)))
        return Qt::SizeFDiagCursor;
    if (((e & Qt::RightEdge) && (e & Qt::TopEdge))
        || ((e & Qt::LeftEdge) && (e & Qt::BottomEdge)))
        return Qt::SizeBDiagCursor;
    if (e & (Qt::LeftEdge | Qt::RightEdge)) return Qt::SizeHorCursor;
    if (e & (Qt::TopEdge | Qt::BottomEdge)) return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

}  // namespace

// ============================================================ WindowButtons

WindowButtons::WindowButtons(QWidget *parent) : QWidget(parent) {
    // Never wider than the three lights: spare room in a header is not theirs.
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *lay = new QHBoxLayout(this);

    auto *close = new TrafficLight(Light::Close, this);
    close->setToolTip(tr("Close"));
    connect(close, &QAbstractButton::clicked, this, [this] { window()->close(); });

    auto *minimise = new TrafficLight(Light::Minimise, this);
    minimise->setToolTip(tr("Minimise"));
    connect(minimise, &QAbstractButton::clicked, this,
            [this] { window()->showMinimized(); });

    auto *maximise = new TrafficLight(Light::Maximise, this);
    maximise->setToolTip(tr("Maximise"));
    connect(maximise, &QAbstractButton::clicked, this, [this] {
        QWidget *top = window();
        if (top->isMaximized()) top->showNormal();
        else top->showMaximized();
    });

    // macOS order, which is Monarchy's: close at the edge.
    lay->addWidget(close);
    lay->addWidget(minimise);
    lay->addWidget(maximise);
    maximise_ = maximise;

    applySettings();
    connect(&Settings::instance(), &Settings::changed, this,
            &WindowButtons::applySettings);
}

void WindowButtons::setMaximiseVisible(bool visible) {
    maximise_->setVisible(visible);
}

void WindowButtons::applySettings() {
    const Settings &s = Settings::instance();
    auto *lay = static_cast<QHBoxLayout *>(layout());
    lay->setSpacing(s.buttonSpacing());
    lay->setContentsMargins(s.buttonPadding(), 0, s.buttonPadding(), 0);
    // Over the layout: TrafficLight has no Q_OBJECT for findChildren to use.
    for (int i = 0; i < lay->count(); ++i)
        if (auto *light = dynamic_cast<TrafficLight *>(lay->itemAt(i)->widget()))
            light->applySettings();
    updateGeometry();
}

// ============================================================== ChromeGroup

void paintRimHighlight(QPainter *painter, const QRectF &bounds, qreal radius,
                       const QColor &chrome) {
    if (bounds.width() < 2.0 || bounds.height() < 2.0) return;
    constexpr qreal kLine = 1.0;
    const bool dark = qGray(chrome.rgb()) < 128;
    // Faint on purpose: a control catching light along its crown, not a
    // border drawn round it.
    const QColor top = dark ? QColor(255, 255, 255, 48) : QColor(255, 255, 255, 110);
    const QColor bottom = dark ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 20);
    const auto alpha = [](QColor c, int a) { c.setAlpha(a); return c; };

    // Half a pen in, so the stroke lands on the shape's edge.
    const QRectF rim = bounds.adjusted(kLine / 2, kLine / 2, -kLine / 2, -kLine / 2);
    const qreal rimRadius = qMax(0.0, radius - kLine / 2);

    QLinearGradient fade(rim.left(), rim.top(), rim.left(), rim.bottom());
    fade.setColorAt(0.00, top);
    fade.setColorAt(0.28, alpha(top, top.alpha() / 3));
    fade.setColorAt(0.50, alpha(top, 0));
    fade.setColorAt(0.72, alpha(bottom, bottom.alpha() / 3));
    fade.setColorAt(1.00, bottom);

    QPainterPath path;
    path.addRoundedRect(rim, rimRadius, rimRadius);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QBrush(fade), kLine));
    painter->drawPath(path);
    painter->restore();
}

ChromeGroup::ChromeGroup(qreal radius, QWidget *parent) : QWidget(parent), radius_(radius) {
    // Without this a plain QWidget ignores the style sheet's background.
    setAttribute(Qt::WA_StyledBackground, true);
}

void ChromeGroup::setChromeColour(const QColor &chrome) {
    chrome_ = chrome;
    update();
}

void ChromeGroup::paintEvent(QPaintEvent *) {
    QStyleOption option;
    option.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &p, this);
    paintRimHighlight(&p, QRectF(rect()), radius_,
                      chrome_.isValid() ? chrome_.get() : palette().color(QPalette::Window));
}

// ============================================================ toolbar pills

QToolButton *toolButton(const QString &glyph, const QString &tip, QWidget *parent) {
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("chromeButton"));
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::TabFocus);
    b->setToolTip(tip);
    b->setAccessibleName(tip);
    // Dimmed, the way Monarchy's toolbars draw their glyphs: the text colour
    // at about 60%, which on this palette is TextDim.
    b->setIcon(Theme::icon(glyph, Theme::TextDim, 18));
    b->setIconSize(QSize(18, 18));
    return b;
}

ChromeGroup *pillGroup(QWidget *parent, std::initializer_list<QAbstractButton *> buttons,
                       const QColor &chrome) {
    auto *group = new ChromeGroup(kGroupHeight / 2.0, parent);
    group->setObjectName(QStringLiteral("actionGroup"));
    group->setFixedHeight(kGroupHeight);
    group->setChromeColour(chrome);
    auto *lay = new QHBoxLayout(group);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->setSizeConstraint(QLayout::SetFixedSize);
    for (QAbstractButton *b : buttons) lay->addWidget(b);
    return group;
}

// ================================================================ frameless

void makeFrameless(QWidget *window) {
    window->setAttribute(Qt::WA_TranslucentBackground, true);
    window->setWindowFlag(Qt::FramelessWindowHint, true);
#ifdef WAVELINE_HAVE_KWINDOWSYSTEM
    new BlurAttachment(window);
    new ShadowAttachment(window);
#endif
}

void prepareResizeStrip(QWidget *central) {
    central->setMouseTracking(true);
    // A widget with no cursor of its own shows its parent's, so every child
    // would carry the resize shape into the middle of the window for as long
    // as it stayed set on `central`.
    for (QObject *child : central->children()) {
        auto *w = qobject_cast<QWidget *>(child);
        if (w && !w->testAttribute(Qt::WA_SetCursor)) w->setCursor(Qt::ArrowCursor);
    }
}

bool handleResizeStrip(QWidget *central, QWindow *handle, QEvent *event) {
    switch (event->type()) {
    case QEvent::MouseMove: {
        const auto *m = static_cast<QMouseEvent *>(event);
        const Qt::Edges edges = resizeEdgesAt(central, m->position().toPoint());
        if (edges) central->setCursor(cursorForEdges(edges));
        else central->unsetCursor();
        return false;
    }
    case QEvent::Leave:
        central->unsetCursor();
        return false;
    case QEvent::MouseButtonPress: {
        const auto *m = static_cast<QMouseEvent *>(event);
        if (m->button() != Qt::LeftButton) return false;
        const Qt::Edges edges = resizeEdgesAt(central, m->position().toPoint());
        // Handed to the compositor: it owns the geometry.
        if (edges && handle) {
            handle->startSystemResize(edges);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool handleTitlebarEvent(QWidget *window, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress) {
        const auto *m = static_cast<QMouseEvent *>(event);
        if (m->button() == Qt::LeftButton && window->windowHandle()) {
            window->windowHandle()->startSystemMove();
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        const auto *m = static_cast<QMouseEvent *>(event);
        if (m->button() == Qt::LeftButton) {
            if (window->isMaximized()) window->showNormal();
            else window->showMaximized();
            return true;
        }
    }
    return false;
}

// ============================================================ WindowChrome

WindowChrome *WindowChrome::adopt(QWidget *window, QBoxLayout *outer) {
    if (!isActive()) return nullptr;
    auto *chrome = new WindowChrome(window, true);

    // Room for the header above the content, and at least the grab strip on
    // the other three sides. The top keeps a little of the gap the window had
    // under its old top edge, less the header's own padding below its pills.
    const QMargins was = outer->contentsMargins();
    const QMargins now(std::max(was.left(), kResizeMargin),
                       kResizeMargin + kHeaderHeight + std::max(0, was.top() - 10),
                       std::max(was.right(), kResizeMargin),
                       std::max(was.bottom(), kResizeMargin));
    outer->setContentsMargins(now);

    // A window sized to its content by hand would otherwise lose the header's
    // height from its content. One sized by its layout adjusts on its own.
    const int dw = now.left() + now.right() - was.left() - was.right();
    const int dh = now.top() + now.bottom() - was.top() - was.bottom();
    if (window->minimumWidth() > 0 && window->minimumWidth() == window->maximumWidth())
        window->setFixedWidth(window->minimumWidth() + dw);
    if (window->minimumHeight() > 0 && window->minimumHeight() == window->maximumHeight())
        window->setFixedHeight(window->minimumHeight() + dh);
    return chrome;
}

WindowChrome *WindowChrome::create(QWidget *window) {
    if (!isActive()) return nullptr;
    return new WindowChrome(window, false);
}

WindowChrome::WindowChrome(QWidget *window, bool overlay)
    : QObject(window), window_(window), overlay_(overlay) {
    makeFrameless(window);

    header_ = new QWidget(window);
    header_->setObjectName(QStringLiteral("monarchyHeader"));
    header_->setFixedHeight(kHeaderHeight);
    header_->installEventFilter(this);
    auto *lay = new QHBoxLayout(header_);
    lay->setContentsMargins(10, 5, 10, 5);
    lay->setSpacing(8);

    buttons_ = new WindowButtons(header_);
    lay->addWidget(buttons_, 0, Qt::AlignVCenter);
    lay->addSpacing(8);

    title_ = new QLabel(window->windowTitle(), header_);
    title_->setObjectName(QStringLiteral("monarchyWindowTitle"));
    // Its own width and no more. A header with nothing of the window's own
    // after the title -- Soundboard Settings' -- would otherwise hand the
    // spare width to the title and the traffic lights, and push the three
    // lights apart across the row.
    title_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    // The name is part of the titlebar: a press on it drags the window.
    title_->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(title_);

    // The rest of the row. adopt() starts it with a stretch, so a window's own
    // buttons land at the right; create() leaves it to the caller, whose old
    // title bar laid the same row out itself.
    toolbar_ = new QHBoxLayout;
    toolbar_->setSpacing(8);
    if (overlay_) toolbar_->addStretch(1);
    lay->addLayout(toolbar_, 1);

    window->installEventFilter(this);
    if (overlay_) {
        window->setMouseTracking(true);
        placeHeader();
    }
    connect(&Settings::instance(), &Settings::changed, window, qOverload<>(&QWidget::update));
}

void WindowChrome::placeHeader() {
    header_->setGeometry(kResizeMargin, kResizeMargin,
                         window_->width() - 2 * kResizeMargin, kHeaderHeight);
    header_->raise();
}

bool WindowChrome::eventFilter(QObject *watched, QEvent *event) {
    if (watched == header_) return handleTitlebarEvent(window_, event);
    if (watched != window_) return false;

    switch (event->type()) {
    case QEvent::Paint: {
        // The window's one tint, under everything in it. Painted here rather
        // than from a style sheet so the window's own sheet, where it has one,
        // is left alone.
        QPainter p(window_);
        QColor tint = Theme::Bg;
        tint.setAlpha(Settings::instance().windowAlpha(!Theme::Light));
        p.fillRect(window_->rect(), tint);
        return false;
    }
    case QEvent::Resize:
        if (overlay_) placeHeader();
        return false;
    case QEvent::WindowTitleChange:
        title_->setText(window_->windowTitle());
        return false;
    case QEvent::Show:
        // A window that put nothing of its own in the header, or nothing that
        // takes the spare width: the spare goes at the end, so the lights and
        // the title sit together at the left as they do in every other
        // header, rather than being spread across the row.
        if (!overlay_) {
            bool hasStretch = false;
            for (int i = 0; i < toolbar_->count(); ++i) {
                if (QSpacerItem *spacer = toolbar_->itemAt(i)->spacerItem();
                    spacer && (spacer->expandingDirections() & Qt::Horizontal))
                    hasStretch = true;
            }
            if (!hasStretch) toolbar_->addStretch(1);
        }
        buttons_->setMaximiseVisible(window_->minimumSize() != window_->maximumSize()
                                     && !(window_->layout() && window_->layout()->sizeConstraint()
                                                                   == QLayout::SetFixedSize));
        if (overlay_) {
            prepareResizeStrip(window_);
            placeHeader();
        }
        return false;
    default:
        break;
    }
    if (overlay_) return handleResizeStrip(window_, window_->windowHandle(), event);
    return false;
}

}  // namespace Monarchy
