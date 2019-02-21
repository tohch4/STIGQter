/*
 * STIGQter - STIG fun with Qt
 *
 * Copyright © 2018–2019 Jon Hood, http://www.hoodsecurity.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "assetview.h"
#include "common.h"
#include "help.h"
#include "stigqter.h"
#include "workerassetadd.h"
#include "workercciadd.h"
#include "workerccidelete.h"
#include "workercklexport.h"
#include "workercklimport.h"
#include "workeremassreport.h"
#include "workerfindingsreport.h"
#include "workerimportemass.h"
#include "workerstigadd.h"
#include "workerstigdelete.h"

#include "ui_stigqter.h"

#include <QThread>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>

/**
 * @class STIGQter
 * @brief @a STIGQter is an open-source STIG Viewer alternative
 * capable of generating findings reports and eMASS-compatible
 * resources.
 *
 * The original goal of STIGQter was to help familiarize the original
 * developer (Jon Hood) with the latest Qt framework (5.12) after
 * leaving the Qt world after version 3.1.
 *
 * After building a STIG Viewer-like, Asset-based interface, members
 * of certain Army SCA-V teams began requesting new features.
 * STIGQter incorporated those features in a faster, open way than
 * DISA's STIG Viewer, and the first beta of the product was released
 * on github.
 *
 * STIGQter now supports eMASS Test Result (TR) imports and exports,
 * and it automates several of the validation tasks in the self-
 * assessment and validation roles of the Army's Risk Management
 * Framework (RMF) process.
 */

/**
 * @brief STIGQter::STIGQter
 * @param parent
 *
 * Main constructor for the main GUI.
 */
STIGQter::STIGQter(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::STIGQter),
    db(new DbManager),
    _updatedAssets(false),
    _updatedCCIs(false),
    _updatedSTIGs(false)
{
    ui->setupUi(this);

    //set the title bar
    this->setWindowTitle(QStringLiteral("STIGQter ") + QStringLiteral(VERSION));

    //make sure that the initial data are populated and active
    EnableInput();
    DisplayCCIs();
    DisplaySTIGs();
    DisplayAssets();

    //remove the close button on the main DB tab.
    ui->tabDB->tabBar()->tabButton(0, QTabBar::RightSide)->resize(0, 0);
}

/**
 * @brief STIGQter::~STIGQter
 *
 * Destructor.
 */
STIGQter::~STIGQter()
{
    CleanThreads();
    delete db;
    delete ui;

}

/**
 * @brief STIGQter::UpdateCCIs
 *
 * Start a thread to update the @a Family, @a Control, and @a CCI
 * information from the NIST and IASE websites. A @a WorkerCCIAdd
 * instance is created.
 */
void STIGQter::UpdateCCIs()
{
    DisableInput();
    _updatedCCIs = true;

    //Create thread to download CCIs and keep GUI active
    auto *t = new QThread;
    auto *c = new WorkerCCIAdd();
    c->moveToThread(t);
    connect(t, SIGNAL(started()), c, SLOT(process()));
    connect(c, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(c, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(c, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(c, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(c);

    t->start();
}

/**
 * @brief STIGQter::OpenCKL
 *
 * Opens the selected @a Asset in a new tab.
 */
void STIGQter::OpenCKL()
{
    foreach(QListWidgetItem *i, ui->lstAssets->selectedItems())
    {
        auto a = i->data(Qt::UserRole).value<Asset>();
        QString assetName = PrintAsset(a);
        for (int j = 0; j < ui->tabDB->count(); j++)
        {
             if (ui->tabDB->tabText(j) == assetName)
             {
                 ui->tabDB->setCurrentIndex(j);
                 return;
             }
        }
        auto *av = new AssetView(a);
        connect(av, SIGNAL(CloseTab(int)), this, SLOT(CloseTab(int)));
        int index = ui->tabDB->addTab(av, assetName);
        av->SetTabIndex(index);
        ui->tabDB->setCurrentIndex(index);
    }
}

/**
 * @brief STIGQter::SelectAsset
 *
 * Show the @a STIGs associated with the selected @a Asset.
 */
void STIGQter::SelectAsset()
{
    UpdateSTIGs();
    EnableInput();
}

/**
 * @brief STIGQter::CleanThreads
 * When the program closes, wait on all background threads to finish
 * processing.
 */
void STIGQter::CleanThreads()
{
    while (!threads.isEmpty())
    {
        QThread *t = threads.takeFirst();
        t->wait();
        delete t;
    }
    foreach (const QObject *o, workers)
    {
        delete o;
    }
    workers.clear();
}

/**
 * @brief STIGQter::CompletedThread
 *
 * When a background thread completes, this function is signaled to
 * update UI elements with the new data.
 */
void STIGQter::CompletedThread()
{
    EnableInput();
    CleanThreads();
    if (_updatedCCIs)
    {
        DisplayCCIs();
        _updatedCCIs = false;
    }
    if (_updatedSTIGs)
    {
        DisplaySTIGs();
        _updatedSTIGs = false;
    }
    if (_updatedAssets)
    {
        DisplayAssets();
        _updatedAssets = false;
    }
    //when maximum <= 0, the progress bar loops
    if (ui->progressBar->maximum() <= 0)
        ui->progressBar->setMaximum(1);
    ui->progressBar->setValue(ui->progressBar->maximum());
}

/**
 * @brief STIGQter::About
 *
 * Display an About @a Help screen.
 */
void STIGQter::About()
{
    Help *h = new Help();
    h->setAttribute(Qt::WA_DeleteOnClose); //clean up after itself (no explicit "delete" needed)
    h->show();
}

/**
 * @brief STIGQter::AddAsset
 *
 * Create a new @a Asset with the selected \a STIGs associated with it.
 */
void STIGQter::AddAsset()
{
    bool ok;
    QString asset = QInputDialog::getText(this, tr("Enter Asset Name"),
                                          tr("Asset:"), QLineEdit::Normal,
                                          QDir::home().dirName(), &ok);
    if (ok)
    {
        DisableInput();
        _updatedAssets = true;
        auto *t = new QThread;
        auto *a = new WorkerAssetAdd();
        Asset tmpAsset;
        tmpAsset.hostName = asset;
        foreach(QListWidgetItem *i, ui->lstSTIGs->selectedItems())
        {
            a->AddSTIG(i->data(Qt::UserRole).value<STIG>());
        }
        a->AddAsset(tmpAsset);
        connect(t, SIGNAL(started()), a, SLOT(process()));
        connect(a, SIGNAL(finished()), t, SLOT(quit()));
        connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
        connect(a, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
        connect(a, SIGNAL(progress(int)), this, SLOT(Progress(int)));
        connect(a, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
        threads.append(t);
        workers.append(a);

        t->start();
    }
}

/**
 * @brief STIGQter::AddSTIGs
 *
 * Adds @a STIG checklists to the database. See
 * @l {https://iase.disa.mil/stigs/Pages/a-z.aspx} for more details.
 */
void STIGQter::AddSTIGs()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this,
        QStringLiteral("Open STIG"), QDir::home().dirName(), QStringLiteral("Compressed STIG (*.zip)"));

    if (fileNames.count() <= 0)
        return; // cancel button pressed

    DisableInput();
    _updatedSTIGs = true;
    auto *t = new QThread;
    auto *s = new WorkerSTIGAdd();
    s->AddSTIGs(fileNames);
    connect(t, SIGNAL(started()), s, SLOT(process()));
    connect(s, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(s, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(s, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(s, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(s);

    t->start();
}

/**
 * @brief STIGQter::CloseTab
 * @param i
 *
 * Close the tab with the identified index.
 */
void STIGQter::CloseTab(int index)
{
    if (ui->tabDB->count() > index)
        ui->tabDB->removeTab(index);
    for (int j = 1; j < ui->tabDB->count(); j++)
    {
        //reset the tab indices for the tabs that were not closed
        dynamic_cast<AssetView*>(ui->tabDB->widget(j))->SetTabIndex(j);
    }
    DisplayAssets();
}

/**
 * @brief STIGQter::DeleteCCIs
 *
 * Clear the database of initial NIST and DISA information.
 */
void STIGQter::DeleteCCIs()
{
    DisableInput();
    _updatedCCIs = true;

    //Create thread to download CCIs and keep GUI active
    auto *t = new QThread;
    auto *c = new WorkerCCIDelete();
    c->moveToThread(t);
    connect(t, SIGNAL(started()), c, SLOT(process()));
    connect(c, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(c, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(c, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(c, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(c);

    t->start();
}

/**
 * @brief STIGQter::DeleteEmass
 *
 * Remove eMASS Test Results (TR) from the database.
 */
void STIGQter::DeleteEmass()
{
    db->DeleteEmassImport();
    EnableInput();
}

/**
 * @brief STIGQter::DeleteSTIGs
 *
 * Remove the selected @a STIGs from the database after making sure
 * that no @a Asset is using them.
 */
void STIGQter::DeleteSTIGs()
{
    DisableInput();
    _updatedSTIGs = true;

    //Create thread to download CCIs and keep GUI active
    auto *t = new QThread;
    auto *s = new WorkerSTIGDelete();
    foreach (QListWidgetItem *i, ui->lstSTIGs->selectedItems())
    {
        STIG s = i->data(Qt::UserRole).value<STIG>();
        db->DeleteSTIG(s);
    }
    s->moveToThread(t);
    connect(t, SIGNAL(started()), s, SLOT(process()));
    connect(s, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(s, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(s, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(s, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(s);

    t->start();
}

/**
 * @brief STIGQter::ExportCKLs
 *
 * Export all possible .ckl files into the selected directory.
 */
void STIGQter::ExportCKLs()
{
    QString dirName = QFileDialog::getExistingDirectory(this, QStringLiteral("Save to Directory"), QDir::home().dirName());

    if (!dirName.isNull() && !dirName.isEmpty())
    {
        DisableInput();
        auto *t = new QThread;
        auto *f = new WorkerCKLExport();
        f->SetExportDir(dirName);
        connect(t, SIGNAL(started()), f, SLOT(process()));
        connect(f, SIGNAL(finished()), t, SLOT(quit()));
        connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
        connect(f, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
        connect(f, SIGNAL(progress(int)), this, SLOT(Progress(int)));
        connect(f, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
        threads.append(t);
        workers.append(f);

        t->start();
    }
}

/**
 * @brief STIGQter::ExportEMASS
 *
 * Create an eMASS Test Result Import workbook.
 */
void STIGQter::ExportEMASS()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        QStringLiteral("Save eMASS Report"), QDir::home().dirName(), QStringLiteral("Microsoft Excel (*.xlsx)"));

    if (fileName.isNull() || fileName.isEmpty())
        return; // cancel button pressed

    DisableInput();
    auto *t = new QThread;
    auto *f = new WorkerEMASSReport();
    f->SetReportName(fileName);
    connect(t, SIGNAL(started()), f, SLOT(process()));
    connect(f, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(f, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(f, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(f, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(f);

    t->start();
}

/**
 * @brief STIGQter::FindingsReport
 *
 * Create a detailed findings report to make the findings data more
 * human-readable.
 */
void STIGQter::FindingsReport()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        QStringLiteral("Save Detailed Findings"), QDir::home().dirName(), QStringLiteral("Microsoft Excel (*.xlsx)"));

    if (fileName.isNull() || fileName.isEmpty())
        return; // cancel button pressed

    DisableInput();
    auto *t = new QThread;
    auto *f = new WorkerFindingsReport();
    f->SetReportName(fileName);
    connect(t, SIGNAL(started()), f, SLOT(process()));
    connect(f, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(f, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(f, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(f, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(f);

    t->start();
}

/**
 * @brief STIGQter::ImportCKLs
 *
 * Import existing CKL files (potentially from STIG Viewer or old
 * versions of STIG Qter).
 */
void STIGQter::ImportCKLs()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this,
        QStringLiteral("Import CKL(s)"), QDir::home().dirName(), QStringLiteral("STIG Checklist (*.ckl)"));

    if (fileNames.count() <= 0)
        return; // cancel button pressed

    DisableInput();
    _updatedAssets = true;
    auto *t = new QThread();
    auto *c = new WorkerCKLImport();
    c->AddCKLs(fileNames);
    connect(t, SIGNAL(started()), c, SLOT(process()));
    connect(c, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(c, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(c, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(c, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(c);

    t->start();
}

/**
 * @brief STIGQter::ImportEMASS
 *
 * Import an existing Test Result Import spreadsheet.
 */
void STIGQter::ImportEMASS()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        QStringLiteral("Import eMASS TRExport"), QDir::home().dirName(), QStringLiteral("Excel Spreadsheet (*.xlsx)"));

    if (fileName.isNull() || fileName.isEmpty())
        return; // cancel button pressed

    DisableInput();

    auto *t = new QThread;
    auto *c = new WorkerImportEMASS();
    c->SetReportName(fileName);
    c->moveToThread(t);
    connect(t, SIGNAL(started()), c, SLOT(process()));
    connect(c, SIGNAL(finished()), t, SLOT(quit()));
    connect(t, SIGNAL(finished()), this, SLOT(CompletedThread()));
    connect(c, SIGNAL(initialize(int, int)), this, SLOT(Initialize(int, int)));
    connect(c, SIGNAL(progress(int)), this, SLOT(Progress(int)));
    connect(c, SIGNAL(updateStatus(QString)), ui->lblStatus, SLOT(setText(QString)));
    threads.append(t);
    workers.append(c);

    t->start();
}

/**
 * @brief STIGQter::SelectSTIG
 *
 * This function is triggered when the @a STIG selection changes.
 */
void STIGQter::SelectSTIG()
{
    //select STIGs to create checklists
    ui->btnCreateCKL->setEnabled(ui->lstSTIGs->selectedItems().count() > 0);
}

/**
 * @brief STIGQter::EnableInput
 *
 * At the end of several background workers' processing task, the
 * EnableInput() routine will allow the GUI elements to interact with
 * the user. This function is used in conjunction with
 * @a DisableInput().
 */
void STIGQter::EnableInput()
{
    QList<Family> f = db->GetFamilies();
    QList<STIG> s = db->GetSTIGs();
    bool isImport = db->IsEmassImport();

    ui->btnImportEmass->setEnabled(!isImport);

    if (f.count() > 0)
    {
        //disable deleting CCIs if STIGs have been imported
        ui->btnClearCCIs->setEnabled(s.count() <= 0);
        ui->btnImportCCIs->setEnabled(false);
        ui->btnImportSTIGs->setEnabled(true);
    }
    else
    {
        ui->btnClearCCIs->setEnabled(false);
        ui->btnImportEmass->setEnabled(false);
        ui->btnImportCCIs->setEnabled(true);
        ui->btnImportSTIGs->setEnabled(false);
    }
    ui->btnClearSTIGs->setEnabled(true);
    ui->btnCreateCKL->setEnabled(true);
    ui->btnDeleteEmassImport->setEnabled(isImport);
    ui->btnFindingsReport->setEnabled(true);
    ui->btnImportCKL->setEnabled(true);
    ui->btnOpenCKL->setEnabled(ui->lstAssets->selectedItems().count() > 0);
    ui->btnQuit->setEnabled(true);
    ui->menubar->setEnabled(true);
    SelectSTIG();
}

/**
 * @brief STIGQter::UpdateSTIGs
 *
 * Update the display of STIGs available in the database.
 */
void STIGQter::UpdateSTIGs()
{
    ui->lstCKLs->clear();
    foreach (QListWidgetItem *i, ui->lstAssets->selectedItems())
    {
        auto a = i->data(Qt::UserRole).value<Asset>();
        foreach (const STIG &s, a.GetSTIGs())
        {
            ui->lstCKLs->addItem(PrintSTIG(s));
        }
    }
}

/**
 * @brief STIGQter::Initialize
 * @param max
 * @param val
 *
 * Initializes the progress bar so that it has @a max steps and is
 * currently at step @a val.
 */
void STIGQter::Initialize(int max, int val)
{
    ui->progressBar->reset();
    ui->progressBar->setMaximum(max);
    ui->progressBar->setValue(val);
}

/**
 * @brief STIGQter::Progress
 * @param val
 *
 * Sets the progress bar to display that it is at step @a val. If a
 * negative number is given, it increments the progress bar by one
 * step.
 */
void STIGQter::Progress(int val)
{
    if (val < 0)
    {
        ui->progressBar->setValue(ui->progressBar->value() + 1);
    }
    else
        ui->progressBar->setValue(val);
}

/**
 * @brief STIGQter::DisableInput
 *
 * Prevent user interaction while background processes are busy.
 */
void STIGQter::DisableInput()
{
    ui->btnClearCCIs->setEnabled(false);
    ui->btnClearSTIGs->setEnabled(false);
    ui->btnCreateCKL->setEnabled(false);
    ui->btnDeleteEmassImport->setEnabled(false);
    ui->btnFindingsReport->setEnabled(false);
    ui->btnImportCCIs->setEnabled(false);
    ui->btnImportCKL->setEnabled(false);
    ui->btnImportEmass->setEnabled(false);
    ui->btnImportSTIGs->setEnabled(false);
    ui->btnOpenCKL->setEnabled(false);
    ui->btnQuit->setEnabled(false);
    ui->menubar->setEnabled(false);
}

/**
 * @brief STIGQter::DisplayAssets
 *
 * Show the list of @a Assets to the user.
 */
void STIGQter::DisplayAssets()
{
    ui->lstAssets->clear();
    foreach(const Asset &a, db->GetAssets())
    {
        QListWidgetItem *tmpItem = new QListWidgetItem(); //memory managed by ui->lstAssets container
        tmpItem->setData(Qt::UserRole, QVariant::fromValue<Asset>(a));
        tmpItem->setText(PrintAsset(a));
        ui->lstAssets->addItem(tmpItem);
    }
}

/**
 * @brief STIGQter::DisplayCCIs
 *
 * Show the list of @a CCIs to the user.
 */
void STIGQter::DisplayCCIs()
{
    ui->lstCCIs->clear();
    foreach(const CCI &c, db->GetCCIs())
    {
        CCI tmpCci = c;
        auto *tmpItem = new QListWidgetItem(); //memory managed by ui->lstCCIs container
        tmpItem->setData(Qt::UserRole, QVariant::fromValue<CCI>(tmpCci));
        tmpItem->setText(PrintCCI(tmpCci));
        ui->lstCCIs->addItem(tmpItem);
    }
}

/**
 * @brief STIGQter::DisplaySTIGs
 *
 * Show the list of @a STIGs to the user. This represents the global
 * @a STIG list in the database representing all @a STIGs that have
 * been imported, not only the @a STIGs for a particular @a Asset.
 */
void STIGQter::DisplaySTIGs()
{
    ui->lstSTIGs->clear();
    foreach(const STIG &s, db->GetSTIGs())
    {
        auto *tmpItem = new QListWidgetItem(); //memory managed by ui->lstSTIGs container
        tmpItem->setData(Qt::UserRole, QVariant::fromValue<STIG>(s));
        tmpItem->setText(PrintSTIG(s));
        ui->lstSTIGs->addItem(tmpItem);
    }
}