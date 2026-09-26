#ifndef PROOFSHEET_H
#define PROOFSHEET_H

#include <QDialog>
#include <QStringList>

class HandwritingLibrary;
class QLabel;
class QPushButton;
class QTableWidget;

/*!
 * \brief 手写数据校对表：列出库里所有字符、样本数、笔画数，并画出手写预览。
 *
 * 用来回答"哪些字有数据、哪些字没有"。传入 \a missingChars 时，
 * 会把缺数据的字一并列出并高亮 —— 这就是"找不到数据时警告"的那份清单。
 */
class HandwritingProofSheet : public QDialog
{
    Q_OBJECT
public:
    HandwritingProofSheet(const HandwritingLibrary *library,
                          const QStringList &missingChars = {},
                          QWidget *parent = nullptr);

private:
    void buildUi(const QStringList &missingChars);

    const HandwritingLibrary *m_library = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPushButton *m_reloadButton = nullptr;

signals:
    //! 用户点了"重新载入手写数据"
    void reloadRequested();
};

#endif // PROOFSHEET_H
