#pragma once

#include <QtWidgets/QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;

class ExplorerRunsReplicationDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit ExplorerRunsReplicationDialog(QWidget* parent = nullptr);

    void loadForJob(qint64 jobId, const QString& battlePlanDescription);

private slots:
    void materializeTasMovieDtm();
    void materializeStartSavestate();

private:
    void setInfoText(const QString& text);
    void setStatusMessage(const QString& message, bool isError = false);
    void materializeObjectToPath(qint64 objectRefId, const QString& suggestedFilename);

    QLabel* statusLabel_ = nullptr;
    QPlainTextEdit* infoText_ = nullptr;
    QPushButton* materializeTasMovieButton_ = nullptr;
    QPushButton* materializeStartSavestateButton_ = nullptr;

    qint64 tasMovieObjectRefId_ = -1;
    qint64 startSavestateObjectRefId_ = -1;
};
