#pragma once
#include <QPlainTextEdit>
#include <QInputMethodEvent>
#include <QFocusEvent>

class AiInputEdit final : public QPlainTextEdit {
public:
    explicit AiInputEdit(QWidget *parent=nullptr) : QPlainTextEdit(parent) {
        setPlaceholderText(tr("向 AI 提问…"));
    }
    bool isComposing() const { return m_composing; }
protected:
    void inputMethodEvent(QInputMethodEvent *event) override {
        m_composing=!event->preeditString().isEmpty();
        setPlaceholderText(m_composing ? QString() : tr("向 AI 提问…"));
        QPlainTextEdit::inputMethodEvent(event);
        viewport()->update();
    }
    void focusOutEvent(QFocusEvent *event) override {
        QPlainTextEdit::focusOutEvent(event);
        m_composing=false;
        setPlaceholderText(tr("向 AI 提问…"));
    }
private:
    bool m_composing=false;
};
