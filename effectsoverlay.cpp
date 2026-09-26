#include "effectsoverlay.h"

#include "texteditor.h"

#include <QPainter>

EffectsOverlay::EffectsOverlay(TextEditor *editor)
    : QWidget(editor->viewport())
    , m_editor(editor)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
}

void EffectsOverlay::setOptions(const EffectRenderOptions &options)
{
    m_options = options;
    update();
}

void EffectsOverlay::paintEvent(QPaintEvent *)
{
    if (!m_editor || !m_options.anyLayer())
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // viewport 坐标 -> 文档坐标：和正文共用 TextEditor::documentToViewport()。
    // 这一点很关键 —— 正文和覆盖层必须用同一个原点，
    // 否则笔迹会整体偏掉一个页边距（曾经就是这个 bug）。
    painter.setTransform(m_editor->documentToViewport());

    renderEffects(&painter, m_editor->document(), m_options);
}
