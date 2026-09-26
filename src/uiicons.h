#pragma once
#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>

// Small, consistent outline icons, painted at the device's actual resolution.
namespace UiIcons {
class Engine final : public QIconEngine {
public:
    Engine(QString name, QColor color) : m_name(std::move(name)), m_color(color) {}
    QIconEngine *clone() const override { return new Engine(m_name, m_color); }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        p->translate(rect.center()); p->scale(rect.width()/24.0, rect.height()/24.0); p->translate(-12,-12);
        QColor c=m_color; if(mode==QIcon::Disabled) c.setAlpha(80);
        p->setPen(QPen(c,1.65,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin)); p->setBrush(Qt::NoBrush);
        auto line=[&](qreal x,qreal y,qreal X,qreal Y){p->drawLine(QPointF(x,y),QPointF(X,Y));};
        if(m_name=="attach") { QPainterPath path; path.moveTo(8,13);path.lineTo(14,7);path.cubicTo(17,4,21,8,18,11);path.lineTo(10,19);path.cubicTo(5,24,0,17,5,12);path.lineTo(14,3);p->drawPath(path); }
        else if(m_name=="outline") { p->drawRoundedRect(QRectF(3,4,18,16),2,2); line(9,4,9,20); line(12,9,18,9); line(12,13,18,13); }
        else if(m_name=="search") {p->drawEllipse(QPointF(10.5,10.5),6,6);line(15,15,20,20);}
        else if(m_name=="prev"||m_name=="next") {const int s=m_name=="prev"?-1:1;line(12-s*3,6,12+s*3,12);line(12+s*3,12,12-s*3,18);}
        else if(m_name=="minus"||m_name=="plus") {line(5,12,19,12);if(m_name=="plus")line(12,5,12,19);}
        else if(m_name=="width") {line(3,5,3,19);line(21,5,21,19);line(7,12,17,12);line(7,12,10,9);line(7,12,10,15);line(17,12,14,9);line(17,12,14,15);}
        else if(m_name=="page") {p->drawRoundedRect(QRectF(7,3,10,18),1,1);line(3,7,3,17);line(21,7,21,17);}
        else if(m_name=="edit") {QPainterPath path;path.moveTo(5,15);path.lineTo(15,5);path.lineTo(19,9);path.lineTo(9,19);path.lineTo(4,20);path.closeSubpath();p->drawPath(path);line(13,7,17,11);}
        else if(m_name=="summary") {p->drawRoundedRect(QRectF(5,3,14,18),2,2);line(9,8,15,8);line(9,12,15,12);line(9,16,12,16);}
        else if(m_name=="sparkle") {QPainterPath path;path.moveTo(12,3);path.quadTo(13,10,20,12);path.quadTo(13,14,12,21);path.quadTo(11,14,4,12);path.quadTo(11,10,12,3);p->drawPath(path);line(20,3,20,7);line(18,5,22,5);}
        else if(m_name=="formula") {line(18,5,7,5);line(7,5,13,12);line(13,12,7,19);line(7,19,18,19);}
        else if(m_name=="book") {QPainterPath path;path.moveTo(12,6);path.quadTo(7,3,3,5);path.lineTo(3,19);path.quadTo(8,17,12,20);path.quadTo(16,17,21,19);path.lineTo(21,5);path.quadTo(17,3,12,6);path.lineTo(12,20);p->drawPath(path);}
        else if(m_name=="image") {p->drawRoundedRect(QRectF(3,4,18,16),2,2);p->drawEllipse(QPointF(8,9),1.5,1.5);line(4,18,10,12);line(10,12,14,16);line(14,16,17,12);line(17,12,21,17);}
        p->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pix(size); pix.fill(Qt::transparent); QPainter p(&pix); paint(&p,QRect(QPoint(),size),mode,state); return pix;
    }
private:
    QString m_name; QColor m_color;
};
inline QIcon icon(const QString &name, const QColor &color) { return QIcon(new Engine(name,color)); }
}
