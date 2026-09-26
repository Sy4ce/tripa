#ifndef FONTSPOOL_H
#define FONTSPOOL_H

#include <QDialog>
#include <QSet>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/*!
 * \brief 「随机字体」的可选字体池。
 *
 * 原来是把系统里所有字体族都当池子，结果随机出来的字体可能根本不适合正文。
 * 这个对话框让用户明确挑出"可以用哪些字体"，随机只在这个集合里发生。
 *
 * 池子会跟着一起存到 QSettings，下次打开还在。
 */
class FontPoolDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FontPoolDialog(const QStringList &allFamilies,
                            const QStringList &selected,
                            QWidget *parent = nullptr);

    //! 用户勾选的字体族
    QStringList selectedFamilies() const;

    //! 从 QSettings 读字体池；没有配置过就返回内置的推荐字体
    static QStringList loadPool();
    //! 把字体池写进 QSettings
    static void savePool(const QStringList &pool);
    //! 默认池：系统里存在的中英文字体，优先常见的那几个
    static QStringList defaultPool(const QStringList &allFamilies);

private:
    void refreshCount();
    void setCheckedForVisible(bool checked);

    QStringList m_allFamilies;
    QListWidget *m_list = nullptr;
    QLineEdit *m_filter = nullptr;
    QLabel *m_countLabel = nullptr;
};

#endif // FONTSPOOL_H
