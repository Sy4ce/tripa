#ifndef EFFECTSOVERLAY_H
#define EFFECTSOVERLAY_H

#include "effectsrenderer.h"

#include <QWidget>

class TextEditor;

/*!
 * \brief 盖在 TextEditor 的 viewport 上的透明绘制层。
 *
 * 只做一件事：把手写 / 扭曲效果按文档坐标画出来。
 * 它不接收鼠标事件（WA_TransparentForMouseEvents），
 * 所以编辑、选中、光标全部照常工作；滚动时由 TextEditor 通知重绘。
 */
class EffectsOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit EffectsOverlay(TextEditor *editor);

    void setOptions(const EffectRenderOptions &options);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    TextEditor *m_editor = nullptr;
    EffectRenderOptions m_options;
};

#endif // EFFECTSOVERLAY_H
