#pragma once

#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QLabel>

class QListWidget;
class QLineEdit;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QPushButton;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;

// QLabel that keeps an original, full-resolution pixmap and rescales it to
// fit its current size (preserving aspect ratio) whenever the widget is
// resized, instead of permanently downsampling the stored image.
class ImageView : public QLabel {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);
    void setImagePath(const QString& path);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rescale();
    QPixmap m_original;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onAddFiles();
    void onAddFolder();
    void onRemoveSelected();
    void onClearAll();
    void onBrowseOutDir();
    void onRun();
    void onCancel();
    void onProcessReadyReadStdout();
    void onProcessReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onOpenResultsFolder();
    void onPreviewSelectionChanged(int row, int col);
    void onLocateCli();

private:
    QString findCliBinary() const;
    void setRunning(bool running);
    void loadSummaryTable(const QString& summaryCsvPath);
    void appendLog(const QString& text, bool isError);
    QString workingDirForRun() const;

    // Input panel
    QListWidget* m_fileList = nullptr;

    // Options panel
    QLineEdit* m_cliPathEdit = nullptr;
    QLineEdit* m_outDirEdit = nullptr;
    QDoubleSpinBox* m_dpiSpin = nullptr;
    QDoubleSpinBox* m_minAreaSpin = nullptr;
    QDoubleSpinBox* m_maxAreaSpin = nullptr;
    QDoubleSpinBox* m_minWidthSpin = nullptr;
    QDoubleSpinBox* m_minLengthSpin = nullptr;
    QDoubleSpinBox* m_coinDiameterSpin = nullptr;
    QComboBox* m_polarityCombo = nullptr;
    QCheckBox* m_watershedCheck = nullptr;
    QDoubleSpinBox* m_seedSeparationSpin = nullptr;
    QDoubleSpinBox* m_seedMergeSpin = nullptr;
    QDoubleSpinBox* m_creaseCloseSpin = nullptr;
    QDoubleSpinBox* m_minSoliditySpin = nullptr;
    QCheckBox* m_excludeBorderCheck = nullptr;
    QCheckBox* m_colorSeedsCheck = nullptr;
    QCheckBox* m_showIdsCheck = nullptr;
    QCheckBox* m_debugCheck = nullptr;
    QSpinBox* m_threadsSpin = nullptr;

    // Actions
    QPushButton* m_runButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_openResultsButton = nullptr;

    // Output
    QPlainTextEdit* m_log = nullptr;
    QTabWidget* m_resultsTabs = nullptr;
    QTableWidget* m_summaryTable = nullptr;
    ImageView* m_imageView = nullptr;

    QProcess* m_process = nullptr;
    QString m_cliPath;
    QString m_lastOutDir;
    QVector<QString> m_lastAnnotatedImages; // row index -> annotated image path
};
