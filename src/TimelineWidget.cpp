/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TimelineWidget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kHandleWidth = 12.0;
constexpr qreal kHandleHitSlop = 6.0;
constexpr qreal kRulerHeight = 16.0;
constexpr qint64 kMinClipMs = 250;

QString tickLabel(qint64 ms, bool withTenths)
{
	qint64 totalSec = ms / 1000;
	qint64 h = totalSec / 3600;
	qint64 m = (totalSec % 3600) / 60;
	qint64 s = totalSec % 60;
	QString base = h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
			     : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
	if (withTenths)
		base += QStringLiteral(".%1").arg((ms % 1000) / 100);
	return base;
}

} // namespace

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setFixedHeight(100);
}

void TimelineWidget::setDuration(qint64 ms)
{
	durationMs = std::max<qint64>(ms, 1);
	inMs = 0;
	outMs = durationMs;
	playMs = 0;
	viewStart = 0;
	viewEnd = durationMs;
	filmstrip.clear();
	filmstripStart = 0;
	filmstripEnd = durationMs;
	update();
}

void TimelineWidget::setFilmstrip(const QVector<QImage> &frames, qint64 coverStartMs, qint64 coverEndMs)
{
	filmstrip = frames;
	filmstripStart = coverStartMs;
	filmstripEnd = std::max(coverEndMs, coverStartMs + 1);
	update();
}

void TimelineWidget::setFilmstripLoading(bool loading)
{
	if (filmstripLoading != loading) {
		filmstripLoading = loading;
		update();
	}
}

void TimelineWidget::clear()
{
	filmstrip.clear();
	filmstripLoading = false;
	durationMs = 0;
	inMs = outMs = playMs = 0;
	viewStart = viewEnd = 0;
	update();
}

void TimelineWidget::setInPoint(qint64 ms)
{
	qint64 v = std::clamp<qint64>(ms, 0, std::max<qint64>(outMs - kMinClipMs, 0));
	if (v != inMs) {
		inMs = v;
		emit inPointChanged(inMs);
		update();
	}
}

void TimelineWidget::setOutPoint(qint64 ms)
{
	qint64 v = std::clamp<qint64>(ms, std::min<qint64>(inMs + kMinClipMs, durationMs), durationMs);
	if (v != outMs) {
		outMs = v;
		emit outPointChanged(outMs);
		update();
	}
}

void TimelineWidget::setPlayhead(qint64 ms)
{
	qint64 v = std::clamp<qint64>(ms, 0, durationMs);
	if (v != playMs) {
		playMs = v;
		update();
	}
}

QRectF TimelineWidget::rulerRect() const
{
	return QRectF(kHandleWidth, 2.0, width() - 2.0 * kHandleWidth, kRulerHeight);
}

QRectF TimelineWidget::stripRect() const
{
	return QRectF(kHandleWidth, kRulerHeight + 4.0, width() - 2.0 * kHandleWidth,
		      height() - kRulerHeight - 10.0);
}

qint64 TimelineWidget::xToMs(qreal x) const
{
	QRectF r = stripRect();
	qint64 span = viewEnd - viewStart;
	if (r.width() <= 0 || span <= 0)
		return 0;
	qreal t = (x - r.left()) / r.width();
	return std::clamp<qint64>(viewStart + (qint64)(t * (qreal)span), 0, durationMs);
}

qreal TimelineWidget::msToX(qint64 ms) const
{
	QRectF r = stripRect();
	qint64 span = std::max<qint64>(viewEnd - viewStart, 1);
	return r.left() + r.width() * ((qreal)(ms - viewStart) / (qreal)span);
}

qint64 TimelineWidget::minViewSpan() const
{
	return std::max<qint64>(500, durationMs / 500);
}

int TimelineWidget::desiredTileCount(double aspect) const
{
	if (aspect <= 0.0)
		aspect = 16.0 / 9.0;
	QRectF r = stripRect();
	double tileW = std::max(r.height() * aspect, 1.0);
	return std::clamp((int)std::floor(r.width() / tileW), 3, 24);
}

void TimelineWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	/* tile count depends on width; drop the stale tiles and let the
	 * owner regenerate (debounced) */
	if (durationMs > 0) {
		filmstrip.clear();
		filmstripLoading = true;
		emit viewChanged(viewStart, viewEnd);
		update();
	}
}

void TimelineWidget::clampView()
{
	qint64 span = std::clamp<qint64>(viewEnd - viewStart, minViewSpan(), durationMs);
	viewStart = std::clamp<qint64>(viewStart, 0, durationMs - span);
	viewEnd = viewStart + span;
}

void TimelineWidget::panBy(qint64 deltaMs)
{
	qint64 span = viewEnd - viewStart;
	viewStart = std::clamp<qint64>(viewStart + deltaMs, 0, durationMs - span);
	viewEnd = viewStart + span;
}

TimelineWidget::Drag TimelineWidget::hitTest(const QPointF &pos) const
{
	if (durationMs <= 0)
		return Drag::None;
	qreal inX = msToX(inMs);
	qreal outX = msToX(outMs);
	if (std::abs(pos.x() - inX) <= kHandleWidth / 2 + kHandleHitSlop)
		return Drag::InHandle;
	if (std::abs(pos.x() - outX) <= kHandleWidth / 2 + kHandleHitSlop)
		return Drag::OutHandle;
	return Drag::Scrub;
}

void TimelineWidget::updateCursor(const QPointF &pos)
{
	Drag hit = dragging != Drag::None ? dragging : hitTest(pos);
	switch (hit) {
	case Drag::InHandle:
	case Drag::OutHandle:
		setCursor(Qt::SizeHorCursor);
		break;
	case Drag::Scrub:
		setCursor(Qt::PointingHandCursor);
		break;
	case Drag::Pan:
		setCursor(Qt::ClosedHandCursor);
		break;
	default:
		unsetCursor();
		break;
	}
}

void TimelineWidget::mousePressEvent(QMouseEvent *event)
{
	if (durationMs <= 0)
		return;
	if (event->button() == Qt::MiddleButton) {
		dragging = Drag::Pan;
		panLastX = event->position().x();
		updateCursor(event->position());
		return;
	}
	if (event->button() != Qt::LeftButton)
		return;
	dragging = hitTest(event->position());
	if (dragging == Drag::InHandle) {
		setInPoint(xToMs(event->position().x()));
		emit handleScrubbed(inMs);
	} else if (dragging == Drag::OutHandle) {
		setOutPoint(xToMs(event->position().x()));
		emit handleScrubbed(outMs);
	} else if (dragging == Drag::Scrub) {
		setPlayhead(xToMs(event->position().x()));
		emit seekRequested(playMs);
	}
	updateCursor(event->position());
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *event)
{
	if (durationMs <= 0)
		return;
	qreal x = event->position().x();
	switch (dragging) {
	case Drag::InHandle:
		setInPoint(xToMs(x));
		emit handleScrubbed(inMs);
		break;
	case Drag::OutHandle:
		setOutPoint(xToMs(x));
		emit handleScrubbed(outMs);
		break;
	case Drag::Scrub:
		setPlayhead(xToMs(x));
		emit seekRequested(playMs);
		break;
	case Drag::Pan: {
		qint64 span = viewEnd - viewStart;
		qint64 oldStart = viewStart;
		qreal dx = x - panLastX;
		panLastX = x;
		panBy((qint64)(-dx * (qreal)span / std::max(stripRect().width(), 1.0)));
		if (viewStart != oldStart) {
			/* stale tiles look wrong while the view moves */
			filmstrip.clear();
			filmstripLoading = true;
			emit viewChanged(viewStart, viewEnd);
		}
		update();
		break;
	}
	case Drag::None:
		break;
	}
	updateCursor(event->position());
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *event)
{
	dragging = Drag::None;
	updateCursor(event->position());
}

void TimelineWidget::wheelEvent(QWheelEvent *event)
{
	if (durationMs <= 0)
		return;
	qint64 span = viewEnd - viewStart;
	qint64 oldStart = viewStart;
	qint64 oldEnd = viewEnd;
	int steps = event->angleDelta().y() / 120;
	if (steps == 0)
		steps = event->angleDelta().y() > 0 ? 1 : -1;

	if (event->modifiers() & Qt::ShiftModifier) {
		panBy(-steps * span / 10);
	} else {
		/* zoom anchored at the cursor */
		qint64 anchor = xToMs(event->position().x());
		double factor = std::pow(1.0 / 1.3, steps);
		qint64 newSpan = std::clamp<qint64>((qint64)((double)span * factor), minViewSpan(), durationMs);
		double frac = span > 0 ? (double)(anchor - viewStart) / (double)span : 0.5;
		viewStart = anchor - (qint64)((double)newSpan * frac);
		viewEnd = viewStart + newSpan;
		clampView();
	}
	if (viewStart != oldStart || viewEnd != oldEnd) {
		/* stale tiles look wrong while the view moves */
		filmstrip.clear();
		filmstripLoading = true;
		emit viewChanged(viewStart, viewEnd);
		update();
	}
	event->accept();
}

void TimelineWidget::leaveEvent(QEvent *)
{
	if (dragging == Drag::None)
		unsetCursor();
}

void TimelineWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	QRectF strip = stripRect();
	QRectF ruler = rulerRect();

	p.fillRect(rect(), QColor(24, 24, 28));

	if (durationMs <= 0) {
		p.setPen(QColor(120, 120, 130));
		p.drawText(rect(), Qt::AlignCenter, tr("No clip loaded"));
		return;
	}

	/* ---- time ruler ---- */
	{
		qint64 span = viewEnd - viewStart;
		static const qint64 stepChoices[] = {100,   250,   500,    1000,   2000,   5000,   10000,
						     15000, 30000, 60000,  120000, 300000, 600000};
		qint64 step = stepChoices[12];
		for (qint64 c : stepChoices) {
			if (span / c <= 10) {
				step = c;
				break;
			}
		}
		QFont f = p.font();
		f.setPointSizeF(7.5);
		p.setFont(f);
		bool tenths = step < 1000;
		for (qint64 t = (viewStart / step) * step; t <= viewEnd; t += step) {
			if (t < viewStart)
				continue;
			qreal x = msToX(t);
			p.setPen(QColor(70, 70, 80));
			p.drawLine(QPointF(x, ruler.bottom() - 4), QPointF(x, strip.bottom()));
			p.setPen(QColor(140, 140, 150));
			p.drawText(QRectF(x + 3, ruler.top(), 64, ruler.height()), Qt::AlignVCenter | Qt::AlignLeft,
				   tickLabel(t, tenths));
		}
	}

	/* ---- filmstrip ---- */
	QPainterPath clipPath;
	clipPath.addRoundedRect(strip, 4, 4);
	p.save();
	p.setClipPath(clipPath);
	p.fillRect(strip, QColor(40, 40, 46));
	if (!filmstrip.isEmpty()) {
		qint64 coverSpan = filmstripEnd - filmstripStart;
		qreal tileDur = (qreal)coverSpan / filmstrip.size();
		qreal stripH = strip.height();
		for (int i = 0; i < filmstrip.size(); i++) {
			qreal xa = msToX(filmstripStart + (qint64)(i * tileDur));
			qreal xb = msToX(filmstripStart + (qint64)((i + 1) * tileDur));
			const QImage &img = filmstrip[i];
			if (xb < strip.left() || xa > strip.right() || img.isNull())
				continue;
			/* Keep the source aspect ratio: center the tile in its
			 * slot, cropped by the slot when narrower, with gaps
			 * when wider. */
			qreal drawW = stripH * (qreal)img.width() / (qreal)std::max(img.height(), 1);
			qreal cx = (xa + xb) / 2.0;
			p.save();
			p.setClipRect(QRectF(xa, strip.top(), xb - xa + 0.5, stripH), Qt::IntersectClip);
			p.drawImage(QRectF(cx - drawW / 2.0, strip.top(), drawW, stripH), img);
			p.restore();
		}
	}

	/* Dim the discarded regions */
	qreal inX = msToX(inMs);
	qreal outX = msToX(outMs);
	QColor dim(10, 10, 12, 190);
	if (inX > strip.left())
		p.fillRect(QRectF(strip.left(), strip.top(), inX - strip.left(), strip.height()), dim);
	if (outX < strip.right())
		p.fillRect(QRectF(outX, strip.top(), strip.right() - outX, strip.height()), dim);

	/* Loading badge */
	if (filmstripLoading) {
		QString msg = tr("Loading preview...");
		QFont f = p.font();
		f.setPointSizeF(9.0);
		p.setFont(f);
		QRectF badge(strip.center().x() - 70, strip.center().y() - 12, 140, 24);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 170));
		p.drawRoundedRect(badge, 5, 5);
		p.setPen(QColor(210, 210, 220));
		p.drawText(badge, Qt::AlignCenter, msg);
	}
	p.restore();

	/* Selection border (bright frame around the kept region) */
	QColor accent(60, 160, 255);
	QRectF sel(inX, strip.top(), outX - inX, strip.height());
	p.save();
	p.setClipRect(QRectF(strip.left() - kHandleWidth, 0, strip.width() + 2 * kHandleWidth, height()));
	p.setPen(QPen(accent, 2.5));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(sel.adjusted(1, 1, -1, -1), 3, 3);

	/* Trim handles */
	auto drawHandle = [&](qreal x) {
		if (x < strip.left() - kHandleWidth || x > strip.right() + kHandleWidth)
			return;
		QRectF h(x - kHandleWidth / 2, strip.top() - 4, kHandleWidth, strip.height() + 8);
		p.setPen(Qt::NoPen);
		p.setBrush(accent);
		p.drawRoundedRect(h, 3, 3);
		p.setPen(QPen(QColor(10, 30, 60), 1.5));
		qreal cx = h.center().x();
		p.drawLine(QPointF(cx - 1.5, h.top() + 8), QPointF(cx - 1.5, h.bottom() - 8));
		p.drawLine(QPointF(cx + 1.5, h.top() + 8), QPointF(cx + 1.5, h.bottom() - 8));
	};
	drawHandle(inX);
	drawHandle(outX);
	p.restore();

	/* Playhead */
	qreal px = msToX(playMs);
	if (px >= strip.left() - 2 && px <= strip.right() + 2) {
		p.setPen(QPen(Qt::white, 2));
		p.drawLine(QPointF(px, strip.top() - 4), QPointF(px, strip.bottom() + 4));
		p.setBrush(Qt::white);
		p.setPen(Qt::NoPen);
		QPolygonF cap;
		cap << QPointF(px - 5, strip.top() - 5) << QPointF(px + 5, strip.top() - 5)
		    << QPointF(px, strip.top() + 2);
		p.drawPolygon(cap);
	}

	/* Zoom indicator: thin bar showing the visible window */
	if (viewEnd - viewStart < durationMs) {
		qreal barY = height() - 4.0;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(60, 60, 70));
		p.drawRoundedRect(QRectF(strip.left(), barY, strip.width(), 3), 1.5, 1.5);
		qreal a = strip.left() + strip.width() * (qreal)viewStart / (qreal)durationMs;
		qreal b = strip.left() + strip.width() * (qreal)viewEnd / (qreal)durationMs;
		p.setBrush(QColor(120, 160, 220));
		p.drawRoundedRect(QRectF(a, barY, std::max(b - a, 6.0), 3), 1.5, 1.5);
	}
}
