#ifndef BASELINEADJUST_H
#define BASELINEADJUST_H

#include "handwriting.h"

#include <QDialog>
#include <QFont>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

/*!
 * \brief 手写笔迹的基线 / 大小调整对话框。
 *
 * 解决的是"手写数据自己不知道该落在哪儿"这件事：
 *   - 采集时字形包围盒的**底边**被当成基线，于是带下伸部的字（g/y/p/q/j）
 *     整字偏高、底部悬在基线上方；
 *   - 包围盒里混进抬笔连线或被切错的笔画时，字会被"塞满字身框"的规则
 *     整体缩小（实测一个字被切进两笔，剩下的笔画被拉小一大截）。
 *
 * 这两件事都没法从数据本身推出来，只能让人对着参考字形看一眼、拖两下。
 * 所以这里按 **CSV 文件** 列表（xml 就是按文件存的），每个字符一行：
 *   基线：相对字身高的比例（%），正值往下沉；
 *   大小：字形缩放倍率（%），100% = 默认行为。
 *
 * 预览里同时画出参考字形（当前工具栏字体）、字格和基线，
 * 所以"底部有没有压住基线、和机打字一样大不大"是一眼就能看出来的。
 *
 * 校正值由 HandwritingLibrary 存到 `<csv 文件名无扩展名>.xml`，
 * 载入 CSV 时自动读回 —— 调一次，以后一直生效。
 */
class HandwritingBaselineDialog : public QDialog
{
    Q_OBJECT
public:
    /*!
     * \param library 手写库（会被就地修改；不接管所有权）
     * \param referenceFontFamily 预览里参考字形的字体（一般给当前工具栏字体）
     * \param pressureToWidth 笔压 -> 线宽系数，和正文同一个口径，
     *        否则预览里的粗细和纸上对不上
     */
    HandwritingBaselineDialog(HandwritingLibrary *library,
                              const QString &referenceFontFamily,
                              double pressureToWidth,
                              QWidget *parent = nullptr);

signals:
    //! 校正值变了：调用方重画正文即可（参数不用重建，库里存的就是同一份）
    void adjustmentsChanged();
    //! 已写盘的 xml（可能在关闭时一次性写出多个文件）
    void adjustmentsSaved(const QStringList &xmlFiles);

private:
    struct Row
    {
        QString source;
        int charIndex = -1;
        HandwritingFileEntry entry;
        QDoubleSpinBox *baselineSpin = nullptr;
        QDoubleSpinBox *sizeSpin = nullptr;
        QTableWidgetItem *previewItem = nullptr;
        QTableWidgetItem *noteItem = nullptr;
    };

    void buildUi();
    void fillFileCombo();
    void rebuildRows();
    //! 按库里的校正值刷新某一行的控件与预览
    void refreshRow(int row);
    void refreshFileInfo();
    void onSpinChanged(int row);
    //! 当前选中的文件（组合框里存的完整路径）
    QString currentSource() const;

    //! 表格里选中的行（按升序、去重）
    QVector<int> selectedRows() const;
    void autoAlignRows(const QVector<int> &rows);
    void resetRows(const QVector<int> &rows);
    //! 按参考字形的墨迹框算出一条校正值；样本太小/不成形时返回 false
    bool autoAdjustFor(const HandwritingSample &sample,
                       const HandwritingFileEntry &entry,
                       HandwritingAdjustment *out) const;

    QPixmap renderPreview(const HandwritingSample &sample,
                          const HandwritingAdjustment &adjust) const;

    /*!
     * 保存（把脏文件写回 xml）。
     * \param quiet true 时不把结果写进摘要栏（关窗口那一次不需要再看一遍）
     */
    bool saveAdjustments(bool quiet);
    /*!
     * accept / reject / 点右上角关闭 三条路的唯一汇合点。
     * 覆盖它而不是 closeEvent()：QDialog 点 X 走的是 done(Rejected)，
     * 只重写 closeEvent 会漏掉 Esc，用户按一下就把刚调的值丢了。
     */
    void done(int result) override;

    HandwritingLibrary *m_library = nullptr;
    QString m_referenceFamily;
    double m_pressureToWidth = 0.20;

    QComboBox *m_fileCombo = nullptr;
    QLabel *m_fileInfoLabel = nullptr;
    QCheckBox *m_referenceCheck = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPushButton *m_alignSelectedButton = nullptr;
    QPushButton *m_alignAllButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_closeButton = nullptr;

    QVector<Row> m_rows;
    //! 正在批量刷新表格（此时的 valueChanged 不算用户改动）
    bool m_populating = false;
    //! 参考字形的字体度量（由 renderPreview 按预览尺寸算好后缓存）
    mutable QFont m_previewFont;
    mutable double m_previewCellH = 0.0;
};

#endif // BASELINEADJUST_H
