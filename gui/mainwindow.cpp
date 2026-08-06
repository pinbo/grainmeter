#include "mainwindow.h"

#include <algorithm>

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>
#include <QFile>
#include <QTextStream>
#include <QHeaderView>
#include <QStatusBar>
#include <QMenuBar>
#include <QResizeEvent>
#include <QRegularExpression>

// ---------------------------------------------------------------- ImageView

ImageView::ImageView(QWidget* parent) : QLabel(parent) {
    setAlignment(Qt::AlignCenter);
    setMinimumSize(200, 200);
    setText("No image loaded");
}

void ImageView::setImagePath(const QString& path) {
    m_original = QPixmap(path);
    rescale();
}

void ImageView::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    rescale();
}

void ImageView::rescale() {
    if (m_original.isNull()) return;
    setPixmap(m_original.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

// --------------------------------------------------------------- MainWindow

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Grainmeter");
    resize(1250, 780);

    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &MainWindow::onProcessReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &MainWindow::onProcessReadyReadStderr);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onProcessFinished);

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* rootLayout = new QHBoxLayout(central);

    // ---------------- Left: inputs + options ----------------
    auto* leftPanel = new QWidget(this);
    leftPanel->setMinimumWidth(340);
    leftPanel->setMaximumWidth(440);
    auto* leftLayout = new QVBoxLayout(leftPanel);

    auto* inputGroup = new QGroupBox("Input images", leftPanel);
    auto* inputLayout = new QVBoxLayout(inputGroup);
    m_fileList = new QListWidget(inputGroup);
    m_fileList->setObjectName("fileList");
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    inputLayout->addWidget(m_fileList);
    auto* inputButtonsGrid = new QGridLayout();
    auto* addFilesBtn = new QPushButton("Add Files...", inputGroup);
    auto* addFolderBtn = new QPushButton("Add Folder...", inputGroup);
    auto* removeBtn = new QPushButton("Remove", inputGroup);
    auto* clearBtn = new QPushButton("Clear", inputGroup);
    inputButtonsGrid->addWidget(addFilesBtn, 0, 0);
    inputButtonsGrid->addWidget(addFolderBtn, 0, 1);
    inputButtonsGrid->addWidget(removeBtn, 1, 0);
    inputButtonsGrid->addWidget(clearBtn, 1, 1);
    inputLayout->addLayout(inputButtonsGrid);
    connect(addFilesBtn, &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(addFolderBtn, &QPushButton::clicked, this, &MainWindow::onAddFolder);
    connect(removeBtn, &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearAll);
    leftLayout->addWidget(inputGroup, /*stretch=*/1);

    auto* optionsGroup = new QGroupBox("Options", leftPanel);
    auto* form = new QFormLayout(optionsGroup);

    auto* cliPathRow = new QWidget(optionsGroup);
    auto* cliPathRowLayout = new QHBoxLayout(cliPathRow);
    cliPathRowLayout->setContentsMargins(0, 0, 0, 0);
    m_cliPathEdit = new QLineEdit(cliPathRow);
    m_cliPathEdit->setReadOnly(true);
    m_cliPathEdit->setPlaceholderText("(not found -- click Browse...)");
    auto* browseCliBtn = new QPushButton("Browse...", cliPathRow);
    cliPathRowLayout->addWidget(m_cliPathEdit);
    cliPathRowLayout->addWidget(browseCliBtn);
    connect(browseCliBtn, &QPushButton::clicked, this, &MainWindow::onLocateCli);
    form->addRow("CLI executable:", cliPathRow);

    auto* outDirRow = new QWidget(optionsGroup);
    auto* outDirRowLayout = new QHBoxLayout(outDirRow);
    outDirRowLayout->setContentsMargins(0, 0, 0, 0);
    m_outDirEdit = new QLineEdit("results", outDirRow);
    auto* browseOutDirBtn = new QPushButton("Browse...", outDirRow);
    outDirRowLayout->addWidget(m_outDirEdit);
    outDirRowLayout->addWidget(browseOutDirBtn);
    connect(browseOutDirBtn, &QPushButton::clicked, this, &MainWindow::onBrowseOutDir);
    form->addRow("Output folder:", outDirRow);

    m_dpiSpin = new QDoubleSpinBox(optionsGroup);
    m_dpiSpin->setRange(50, 2400);
    m_dpiSpin->setValue(300);
    m_dpiSpin->setSuffix(" dpi");
    form->addRow("Scan resolution:", m_dpiSpin);

    m_coinDiameterSpin = new QDoubleSpinBox(optionsGroup);
    m_coinDiameterSpin->setRange(0.0, 100.0);
    m_coinDiameterSpin->setSingleStep(0.1);
    m_coinDiameterSpin->setValue(0.0);
    m_coinDiameterSpin->setSuffix(" mm");
    m_coinDiameterSpin->setSpecialValueText("0 (off, use DPI)");
    m_coinDiameterSpin->setToolTip(
        "Place a coin (or other reference circle) next to the grains in\n"
        "the scan. The biggest circular blob found in the image is\n"
        "assumed to be it; its pixel diameter is measured and used to\n"
        "derive px-per-mm, overriding the scan resolution above. The\n"
        "coin itself is excluded from grain counting.");
    form->addRow("Coin diameter (overrides DPI):", m_coinDiameterSpin);

    m_minAreaSpin = new QDoubleSpinBox(optionsGroup);
    m_minAreaSpin->setRange(0, 10000);
    m_minAreaSpin->setValue(3.0);
    m_minAreaSpin->setSuffix(" mm\u00B2");
    form->addRow("Min grain area:", m_minAreaSpin);

    m_maxAreaSpin = new QDoubleSpinBox(optionsGroup);
    m_maxAreaSpin->setRange(0, 10000);
    m_maxAreaSpin->setValue(20.0);
    m_maxAreaSpin->setSuffix(" mm\u00B2");
    m_maxAreaSpin->setToolTip(
        "Regions bigger than this are assumed to be an unsplit\n"
        "touching-grain cluster: they get a forced re-split attempt\n"
        "(tighter seed spacing) rather than being discarded. If no\n"
        "split is found, the region is kept as-is (not dropped).");
    form->addRow("Max grain area (re-split above this):", m_maxAreaSpin);

    m_minWidthSpin = new QDoubleSpinBox(optionsGroup);
    m_minWidthSpin->setRange(0, 1000);
    m_minWidthSpin->setValue(1.0);
    m_minWidthSpin->setSuffix(" mm");
    form->addRow("Min grain width:", m_minWidthSpin);

    m_minLengthSpin = new QDoubleSpinBox(optionsGroup);
    m_minLengthSpin->setRange(0, 1000);
    m_minLengthSpin->setValue(2.0);
    m_minLengthSpin->setSuffix(" mm");
    form->addRow("Min grain length:", m_minLengthSpin);

    m_polarityCombo = new QComboBox(optionsGroup);
    m_polarityCombo->addItems({"auto", "dark", "light"});
    form->addRow("Grain vs. background:", m_polarityCombo);

    m_threadsSpin = new QSpinBox(optionsGroup);
    m_threadsSpin->setRange(0, 256);
    m_threadsSpin->setValue(0);
    m_threadsSpin->setSpecialValueText("auto (CPU cores)");
    form->addRow("Threads:", m_threadsSpin);

    m_watershedCheck = new QCheckBox("Split touching grains (watershed)", optionsGroup);
    m_watershedCheck->setChecked(true);
    form->addRow(m_watershedCheck);

    m_seedSeparationSpin = new QDoubleSpinBox(optionsGroup);
    m_seedSeparationSpin->setRange(0.1, 50.0);
    m_seedSeparationSpin->setSingleStep(0.1);
    m_seedSeparationSpin->setValue(2.0);
    m_seedSeparationSpin->setSuffix(" mm");
    m_seedSeparationSpin->setToolTip(
        "Minimum distance between distinct watershed seed peaks.\n"
        "Lower if touching grains are being merged into one;\n"
        "raise if one grain is being split into more than one.");
    form->addRow("Seed separation:", m_seedSeparationSpin);

    m_seedMergeSpin = new QDoubleSpinBox(optionsGroup);
    m_seedMergeSpin->setRange(0.0, 20.0);
    m_seedMergeSpin->setSingleStep(0.1);
    m_seedMergeSpin->setValue(2.0);
    m_seedMergeSpin->setSuffix(" mm");
    m_seedMergeSpin->setToolTip(
        "Merges nearby near-tied seed points into one.\n"
        "Try raising this first if a single grain shows up as\n"
        "2+ points in debug_sure_fg.png.");
    form->addRow("Seed merge distance:", m_seedMergeSpin);

    m_creaseCloseSpin = new QDoubleSpinBox(optionsGroup);
    m_creaseCloseSpin->setRange(0.0, 20.0);
    m_creaseCloseSpin->setSingleStep(0.1);
    m_creaseCloseSpin->setValue(0.0);
    m_creaseCloseSpin->setSuffix(" mm");
    m_creaseCloseSpin->setToolTip(
        "Bridges a grain's own crease/notch before it can register\n"
        "as two seed peaks (e.g. a wheat kernel's sulcus). Off (0)\n"
        "by default. Raise (e.g. 0.6) if a visible crease is\n"
        "splitting grains into two; lower back down if grains that\n"
        "genuinely touch are being merged into one.");
    form->addRow("Crease closing:", m_creaseCloseSpin);

    m_minSoliditySpin = new QDoubleSpinBox(optionsGroup);
    m_minSoliditySpin->setRange(0.0, 1.0);
    m_minSoliditySpin->setSingleStep(0.05);
    m_minSoliditySpin->setValue(0.0);
    m_minSoliditySpin->setSpecialValueText("0 (off)");
    m_minSoliditySpin->setToolTip(
        "Regions less convex/oval-shaped than this (contour area /\n"
        "convex-hull area) get a forced re-split attempt even if\n"
        "their area looks normal -- catches touching pairs that sum\n"
        "to a plausible single-grain area. Off (0) by default: real\n"
        "single grains commonly measure 0.80-0.95 due to natural\n"
        "texture/crease/asymmetry, so getting this threshold wrong\n"
        "risks mis-splitting ordinary grains. Try 0.75 if you turn\n"
        "it on. See the README for how this default was chosen.");
    form->addRow("Min solidity (shape check):", m_minSoliditySpin);

    m_excludeBorderCheck = new QCheckBox("Exclude grains touching image edge", optionsGroup);
    m_excludeBorderCheck->setChecked(true);
    form->addRow(m_excludeBorderCheck);

    m_colorSeedsCheck = new QCheckBox("Distinct color per grain (GrainScan-style)", optionsGroup);
    form->addRow(m_colorSeedsCheck);

    m_showIdsCheck = new QCheckBox("Show grain id numbers", optionsGroup);
    form->addRow(m_showIdsCheck);

    m_debugCheck = new QCheckBox("Debug (save intermediate mask images)", optionsGroup);
    form->addRow(m_debugCheck);

    leftLayout->addWidget(optionsGroup);

    auto* runRow = new QHBoxLayout();
    m_runButton = new QPushButton("Run", leftPanel);
    m_runButton->setObjectName("runButton");
    m_runButton->setStyleSheet("font-weight: bold; padding: 6px;");
    m_cancelButton = new QPushButton("Cancel", leftPanel);
    m_cancelButton->setEnabled(false);
    runRow->addWidget(m_runButton);
    runRow->addWidget(m_cancelButton);
    leftLayout->addLayout(runRow);
    connect(m_runButton, &QPushButton::clicked, this, &MainWindow::onRun);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::onCancel);

    m_openResultsButton = new QPushButton("Open Results Folder", leftPanel);
    m_openResultsButton->setEnabled(false);
    leftLayout->addWidget(m_openResultsButton);
    connect(m_openResultsButton, &QPushButton::clicked, this, &MainWindow::onOpenResultsFolder);

    rootLayout->addWidget(leftPanel);

    // ---------------- Right: log + results ----------------
    auto* splitter = new QSplitter(Qt::Vertical, this);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    QFont mono("Menlo");
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(10);
    m_log->setFont(mono);
    splitter->addWidget(m_log);

    m_resultsTabs = new QTabWidget(this);

    m_summaryTable = new QTableWidget(this);
    m_summaryTable->setObjectName("summaryTable");
    m_summaryTable->setColumnCount(15);
    m_summaryTable->setHorizontalHeaderLabels(
        {"File", "Seeds", "Mean Area (mm\u00B2)", "Mean Length (mm)", "Mean Width (mm)",
         "Mean Perimeter (mm)", "Mean Circularity", "Mean Aspect Ratio", "Mean R", "Mean G", "Mean B",
         "Mean H", "Mean S", "Mean V", "Oversized Before Split"});
    m_summaryTable->horizontalHeader()->setStretchLastSection(false);
    m_summaryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_summaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_summaryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(m_summaryTable, &QTableWidget::cellClicked, this, &MainWindow::onPreviewSelectionChanged);
    m_resultsTabs->addTab(m_summaryTable, "Summary");

    m_imageView = new ImageView(this);
    m_resultsTabs->addTab(m_imageView, "Preview");

    splitter->addWidget(m_resultsTabs);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    rootLayout->addWidget(splitter, /*stretch=*/1);

    // ---------------- Menu ----------------
    auto* fileMenu = menuBar()->addMenu("&File");
    auto* quitAction = fileMenu->addAction("Quit");
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    statusBar()->showMessage("Ready");

    m_cliPath = findCliBinary();
    m_cliPathEdit->setText(m_cliPath);
    if (m_cliPath.isEmpty()) {
        statusBar()->showMessage(
            "grainmeter CLI not found automatically -- use the \"CLI executable\" "
            "Browse... button in the Options panel");
    } else {
        statusBar()->showMessage("Using CLI: " + m_cliPath);
    }
}

QString MainWindow::findCliBinary() const {
    QSettings settings("grainmeter", "grainmeter-gui");
    QString saved = settings.value("cliPath").toString();
    if (!saved.isEmpty() && QFileInfo::exists(saved)) return saved;

    const QString exeName =
#ifdef Q_OS_WIN
        "grainmeter.exe";
#else
        "grainmeter";
#endif

    QDir appDir(QCoreApplication::applicationDirPath());
    QStringList candidates = {
        appDir.filePath(exeName),
        appDir.filePath("../" + exeName),
        appDir.filePath("../../build/" + exeName),
        appDir.filePath("../build/" + exeName),
    };
    for (const auto& c : candidates) {
        if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
    }

    QString onPath = QStandardPaths::findExecutable(exeName);
    if (!onPath.isEmpty()) return onPath;

    return QString();
}

void MainWindow::onAddFiles() {
    // DontUseNativeDialog: on macOS, an app bundle that hasn't been run
    // through macdeployqt (so it doesn't carry its own bundled Qt
    // plugins/frameworks) can fail to attach the native Cocoa file panel
    // to the window -- the dialog call returns immediately with nothing
    // shown, which looks exactly like the button doing nothing. Qt's own
    // built-in dialog doesn't depend on that native integration at all.
    QStringList files = QFileDialog::getOpenFileNames(
        this, "Select scanned grain images", QString(),
        "Images (*.jpg *.jpeg *.png *.tif *.tiff);;All files (*)",
        nullptr, QFileDialog::DontUseNativeDialog);
    for (const auto& f : files) {
        if (m_fileList->findItems(f, Qt::MatchExactly).isEmpty()) {
            m_fileList->addItem(f);
        }
    }
}

void MainWindow::onAddFolder() {
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select folder of scanned images", QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (dir.isEmpty()) return;
    QDir d(dir);
    QStringList filters = {"*.jpg", "*.jpeg", "*.png", "*.tif", "*.tiff",
                            "*.JPG", "*.JPEG", "*.PNG", "*.TIF", "*.TIFF"};
    QStringList found = d.entryList(filters, QDir::Files, QDir::Name);
    if (found.isEmpty()) {
        QMessageBox::information(this, "No images found",
                                  "No .jpg/.png/.tif images were found in that folder.");
        return;
    }
    for (const auto& f : found) {
        QString full = d.absoluteFilePath(f);
        if (m_fileList->findItems(full, Qt::MatchExactly).isEmpty()) {
            m_fileList->addItem(full);
        }
    }
}

void MainWindow::onRemoveSelected() {
    for (auto* item : m_fileList->selectedItems()) {
        delete m_fileList->takeItem(m_fileList->row(item));
    }
}

void MainWindow::onClearAll() {
    m_fileList->clear();
}

void MainWindow::onBrowseOutDir() {
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select output folder", m_outDirEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (!dir.isEmpty()) m_outDirEdit->setText(dir);
}

void MainWindow::onLocateCli() {
    QString path = QFileDialog::getOpenFileName(
        this, "Locate the grainmeter CLI executable", QString(), QString(),
        nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return;
    m_cliPath = path;
    m_cliPathEdit->setText(path);
    QSettings settings("grainmeter", "grainmeter-gui");
    settings.setValue("cliPath", path);
    statusBar()->showMessage("Using CLI: " + m_cliPath);
}

QString MainWindow::workingDirForRun() const {
    if (m_fileList->count() == 0) return QDir::currentPath();
    QString first = m_fileList->item(0)->text();
    return QFileInfo(first).absolutePath();
}

void MainWindow::onRun() {
    if (m_fileList->count() == 0) {
        QMessageBox::warning(this, "No input files", "Add at least one image first.");
        return;
    }
    if (m_cliPath.isEmpty() || !QFileInfo::exists(m_cliPath)) {
        m_cliPath = findCliBinary();
        m_cliPathEdit->setText(m_cliPath);
        if (m_cliPath.isEmpty()) {
            QMessageBox::warning(this, "grainmeter CLI not found",
                                  "Couldn't find the grainmeter command-line executable.\n\n"
                                  "Use the \"CLI executable\" Browse... button in the Options "
                                  "panel to point to it (the binary produced by building the "
                                  "main CMake project).");
            return;
        }
    }

    QStringList args;
    args << "--input";
    for (int i = 0; i < m_fileList->count(); ++i) {
        args << m_fileList->item(i)->text();
    }
    args << "--dpi" << QString::number(m_dpiSpin->value());
    args << "--out-dir" << m_outDirEdit->text();
    args << "--min-area-mm2" << QString::number(m_minAreaSpin->value());
    args << "--max-area-mm2" << QString::number(m_maxAreaSpin->value());
    args << "--min-width-mm" << QString::number(m_minWidthSpin->value());
    args << "--min-length-mm" << QString::number(m_minLengthSpin->value());
    if (m_coinDiameterSpin->value() > 0.0) args << "--coin-diameter-mm" << QString::number(m_coinDiameterSpin->value());
    args << "--polarity" << m_polarityCombo->currentText();
    if (!m_watershedCheck->isChecked()) args << "--no-watershed";
    args << "--seed-separation-mm" << QString::number(m_seedSeparationSpin->value());
    args << "--seed-merge-mm" << QString::number(m_seedMergeSpin->value());
    args << "--crease-close-mm" << QString::number(m_creaseCloseSpin->value());
    if (m_minSoliditySpin->value() > 0.0) args << "--min-solidity" << QString::number(m_minSoliditySpin->value());
    if (!m_excludeBorderCheck->isChecked()) args << "--include-border";
    if (m_colorSeedsCheck->isChecked()) args << "--color-seeds";
    if (m_showIdsCheck->isChecked()) args << "--show-ids";
    if (m_threadsSpin->value() > 0) args << "--threads" << QString::number(m_threadsSpin->value());
    if (m_debugCheck->isChecked()) args << "--debug";

    m_log->clear();
    m_summaryTable->setRowCount(0);
    m_imageView->setImagePath(QString()); // clear preview
    m_lastAnnotatedImages.clear();

    QString workDir = workingDirForRun();
    m_process->setWorkingDirectory(workDir);

    // Resolve the out-dir the same way the CLI's default-naming would, so
    // we know where to look for results afterwards, relative to workDir if
    // the user left it relative.
    QDir wd(workDir);
    m_lastOutDir = QFileInfo(m_outDirEdit->text()).isAbsolute()
                       ? m_outDirEdit->text()
                       : wd.filePath(m_outDirEdit->text());

    appendLog(QString("Running: %1 %2\n(working dir: %3)\n\n")
                  .arg(m_cliPath, args.join(' '), workDir),
              false);

    setRunning(true);
    m_process->start(m_cliPath, args);
}

void MainWindow::onCancel() {
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        appendLog("\n--- Cancelled by user ---\n", true);
    }
}

void MainWindow::onProcessReadyReadStdout() {
    appendLog(QString::fromLocal8Bit(m_process->readAllStandardOutput()), false);
}

void MainWindow::onProcessReadyReadStderr() {
    appendLog(QString::fromLocal8Bit(m_process->readAllStandardError()), true);
}

void MainWindow::appendLog(const QString& text, bool isError) {
    if (text.isEmpty()) return;
    Q_UNUSED(isError);
    m_log->moveCursor(QTextCursor::End);
    m_log->insertPlainText(text);
    m_log->moveCursor(QTextCursor::End);
}

void MainWindow::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    setRunning(false);
    if (status == QProcess::CrashExit) {
        appendLog("\n--- Process crashed ---\n", true);
    } else {
        appendLog(QString("\n--- Finished (exit code %1) ---\n").arg(exitCode), exitCode != 0);
    }

    QString summaryPath = QDir(m_lastOutDir).filePath("summary.csv");
    if (QFileInfo::exists(summaryPath)) {
        loadSummaryTable(summaryPath);
        m_openResultsButton->setEnabled(true);
        statusBar()->showMessage("Done. Results in " + m_lastOutDir);
    } else {
        statusBar()->showMessage("Finished, but no summary.csv was found -- check the log for errors.");
    }
}

// Minimal CSV line splitter that understands one leading double-quoted
// field (our summary.csv only quotes the filename column) and otherwise
// splits on commas.
static QStringList splitSummaryLine(const QString& line) {
    QStringList fields;
    if (line.startsWith('"')) {
        int endQuote = line.indexOf('"', 1);
        while (endQuote > 0 && endQuote + 1 < line.size() && line[endQuote + 1] == '"') {
            endQuote = line.indexOf('"', endQuote + 2); // skip escaped "" 
        }
        if (endQuote < 0) endQuote = line.size() - 1;
        QString name = line.mid(1, endQuote - 1);
        name.replace("\"\"", "\"");
        fields << name;
        QString rest = line.mid(endQuote + 1);
        if (rest.startsWith(',')) rest = rest.mid(1);
        fields << rest.split(',');
    } else {
        fields = line.split(',');
    }
    return fields;
}

void MainWindow::loadSummaryTable(const QString& summaryCsvPath) {
    QFile f(summaryCsvPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);

    m_summaryTable->setRowCount(0);
    m_lastAnnotatedImages.clear();

    bool first = true;
    int row = 0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (first) { first = false; continue; } // header
        if (line.trimmed().isEmpty()) continue;

        QStringList fields = splitSummaryLine(line);
        if (fields.size() < 14) continue;

        m_summaryTable->insertRow(row);
        QString filePath = fields[0];
        int numCols = std::min(static_cast<int>(fields.size()), 15);
        for (int col = 0; col < numCols; ++col) {
            auto* item = new QTableWidgetItem(fields[col]);
            m_summaryTable->setItem(row, col, item);
        }

        // Figure out where the annotated image for this file ended up:
        // <out-dir>/<stem>_annotated<ext>
        QFileInfo fi(filePath);
        QString ext = fi.suffix();
        if (ext.isEmpty()) ext = "jpg";
        QString annotated = QDir(m_lastOutDir).filePath(fi.completeBaseName() + "_annotated." + ext);
        m_lastAnnotatedImages.append(QFileInfo::exists(annotated) ? annotated : QString());

        ++row;
    }

    if (row > 0) {
        m_summaryTable->selectRow(0);
        onPreviewSelectionChanged(0, 0);
        m_resultsTabs->setCurrentWidget(m_summaryTable);
    }
}

void MainWindow::onPreviewSelectionChanged(int row, int /*col*/) {
    if (row < 0 || row >= m_lastAnnotatedImages.size()) return;
    const QString& path = m_lastAnnotatedImages[row];
    if (!path.isEmpty()) {
        m_imageView->setImagePath(path);
    } else {
        m_imageView->setText("No annotated image available for this file.");
    }
}

void MainWindow::onOpenResultsFolder() {
    if (!m_lastOutDir.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastOutDir));
    }
}

void MainWindow::setRunning(bool running) {
    m_runButton->setEnabled(!running);
    m_cancelButton->setEnabled(running);
    m_fileList->setEnabled(!running);
    statusBar()->showMessage(running ? "Running..." : "Ready");
}
