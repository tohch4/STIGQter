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

#include "dbmanager.h"
#include "cklcheck.h"
#include "common.h"

#include <cstdlib>
#include <QCoreApplication>
#include <QFile>
#include <QMessageBox>
#include <QSqlQuery>
#include <QSqlError>
#include <QThread>
#include <QSqlField>
#include <QSqlDriver>

/*!
 * \class DbManager
 * \brief DbManager::DbManager represents the data layer for the
 * application.
 *
 * Each instance of the \a DbManager uses a thread-specific
 * connection to the SQLite database. Before executing queries, each
 * function checks the connection to the database by checking if the
 * current thread currently has a connection. If it does, the
 * thread's connection is reused. If not, a new, parallel connection
 * is established.
 *
 * Upon successful connection to the database, the version of the
 * database is checked to make sure that it is the latest version.
 *
 * The Semantic Versioning 2.0.0 system is utilized with the database
 * version being the main driver. While in beta (0.1.x), database
 * consistency is not kept. This means that databases built using
 * version 0.1.0_beta of STIGQter are incompatible with 0.1.1_beta.
 * Once released, the database will automatically be upgraded for
 * each new STIGQter revision (for major releases). For example, a
 * database built with STIGQter 1.0.0 would be compatible with
 * STIGQter 1.5.3. However, a database built with STIGQter 1.5.3 may
 * have features unsupported by STIGQter 1.0.0. The constructor for
 * the DbManager handles the automatic detection and upgrade of the
 * database.
 */

/*!
 * \brief DbManager::DbManager
 *
 * Default constructor.
 */
DbManager::DbManager() : DbManager(QString::number(reinterpret_cast<quint64>(QThread::currentThreadId()))) { }

/*!
 * \overload DbManager::DbManager()
 * \brief DbManager::DbManager
 * \param connectionName
 *
 * Overloaded constructor with current thread's connection already
 * provided.
 */
DbManager::DbManager(const QString& connectionName) : DbManager(QCoreApplication::applicationDirPath() + "/STIGQter.db", connectionName) { }


/*!
 * \overload DbManager::DbManager()
 * \brief DbManager::DbManager
 * \param path
 * \param connectionName
 *
 * Overloaded constructor with path to SQLite DB and current thread's
 * connection already provided.
 */
DbManager::DbManager(const QString& path, const QString& connectionName)
{
    bool initialize = false;
    _delayCommit = false;

    //check if database file exists or create it
    if (!QFile::exists(path))
        initialize = true;

    //open SQLite Database
    QSqlDatabase db = QSqlDatabase::database(connectionName);
    if (!db.isValid())
    {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(path);
    }

    if (!db.open())
    {
        Warning(QStringLiteral("Unable to Open DB"), "Unable to open DB " + path);
    }

    if (initialize)
        UpdateDatabaseFromVersion(0);

    int version = GetVariable(QStringLiteral("version")).toInt();
    UpdateDatabaseFromVersion(version);
}

/*!
 * \brief DbManager::~DbManager
 *
 * The destructor verifies that all changes have been written to the
 * database and closes its connection.
 */
DbManager::~DbManager()
{
    if (_delayCommit)
    {
        QSqlDatabase db;
        if (this->CheckDatabase(db))
        {
            db.commit();
        }
    }
}

/*!
 * \brief DbManager::DelayCommit
 * \param delay
 *
 * When engaging in a large quantity of writes, the data may be
 * buffered in memory without committing the changes to the database
 * temporarily by setting \a delay to \c true. Setting \a delay to
 * \c false or destructing the database connection will commit the
 * changes that have been buffered.
 *
 * Developers should be cautious: buffered changes may not show up in
 * parallel threads or in threads that are executing and need the
 * data that have not yet been committed.
 */
void DbManager::DelayCommit(bool delay)
{
    if (delay)
    {
        QSqlDatabase db;
        if (this->CheckDatabase(db))
        {
            QSqlQuery(QStringLiteral("PRAGMA journal_mode = OFF"), db);
            QSqlQuery(QStringLiteral("PRAGMA synchronous = OFF"), db);
        }
    }
    else
    {
        QSqlDatabase db;
        if (this->CheckDatabase(db))
        {
            QSqlQuery(QStringLiteral("PRAGMA journal_mode = ON"), db);
            QSqlQuery(QStringLiteral("PRAGMA synchronous = ON"), db);
            db.commit();
        }
    }
    _delayCommit = delay;
}

/*!
 * \brief DbManager::AddAsset
 * \param asset
 * \return \c True when the \a Asset is added to the database,
 * \c false when the \a Asset is already part of the database or has
 * not been added.
 *
 * To add a new \a Asset to the database, a new \a Asset instance is
 * created in code and sent to this function. If the \a Asset has the
 * default \a id (or an \a id less than or equal to 0), it is assumed
 * to not be part of the database and is committed. On commit, the
 * provided \a Asset's \a id is set to the newly inserted record's
 * \a id.
 *
 * \a Assets must be uniquely named. A single Asset can have multiple
 * \a STIGs that are performed against it. A single computing node
 * usually qualifies as an \a Asset, and the individual components it
 * contains (the OS, applications, custom devices) have \a STIGs that
 * correspond to them. The hierarchy is
 * \a Asset → \a STIG → \a STIGCheck.
 *
 * Example: A single desktop computer will often have the following
 * \a STIGs: Windows 10, Internet Explorer, Microsoft Office (and its
 * subcomponents), FireFox, JRE, and Adobe.
 */
bool DbManager::AddAsset(Asset &asset)
{
    QSqlDatabase db;
    bool ret = false;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);

        //check if Asset exists in the database
        q.prepare(QStringLiteral("SELECT count(*) FROM Asset WHERE hostName = :hostName"));
        q.bindValue(QStringLiteral(":hostName"), asset.hostName);
        q.exec();
        if (q.next())
        {
            if (q.value(0).toInt() > 0)
            {
                Warning(QStringLiteral("Asset Already Exists"), "The Asset " + PrintAsset(asset) + " already exists in the database.");
                return false;
            }
        }
        q.prepare(QStringLiteral("INSERT INTO Asset (`assetType`, `hostName`, `hostIP`, `hostMAC`, `hostFQDN`, `techArea`, `targetKey`, `webOrDatabase`, `webDBSite`, `webDBInstance`) VALUES(:assetType, :hostName, :hostIP, :hostMAC, :hostFQDN, :techArea, :targetKey, :webOrDatabase, :webDBSite, :webDBInstance)"));
        q.bindValue(QStringLiteral(":assetType"), asset.assetType);
        q.bindValue(QStringLiteral(":hostName"), asset.hostName);
        q.bindValue(QStringLiteral(":hostIP"), asset.hostIP);
        q.bindValue(QStringLiteral(":hostMAC"), asset.hostMAC);
        q.bindValue(QStringLiteral(":hostFQDN"), asset.hostFQDN);
        q.bindValue(QStringLiteral(":techArea"), asset.techArea);
        q.bindValue(QStringLiteral(":targetKey"), asset.targetKey);
        q.bindValue(QStringLiteral(":webOrDatabase"), asset.webOrDB);
        q.bindValue(QStringLiteral(":webDBSite"), asset.webDbSite);
        q.bindValue(QStringLiteral(":webDBInstance"), asset.webDbInstance);
        ret = q.exec();
        db.commit();
        asset.id = q.lastInsertId().toInt();
    }
    return ret;
}

/*!
 * \brief DbManager::AddCCI
 * \param cci
 * \return \c True when the \a CCI is added to the database,
 * \c false when the \a CCI is already part of the database or has
 * not been added.
 *
 * To add a new \a CCI to the database, a new \a CCI instance is
 * created in code and sent to this function. If the \a CCI has the
 * default \a id (or an \a id less than or equal to 0), it is assumed
 * to not be part of the database and is committed. On commit, the
 * provided \a CCI's \a id is set to the newly inserted record's
 * \a id.
 */
bool DbManager::AddCCI(CCI &cci)
{
    QSqlDatabase db;
    bool ret = false;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);

        //check if CCI already exists in the DB
        q.prepare(QStringLiteral("SELECT count(*) FROM CCI WHERE cci = :cci"));
        q.bindValue(QStringLiteral(":cci"), cci.cci);
        q.exec();
        if (q.next())
        {
            if (q.value(0).toInt() > 0)
            {
                Warning(QStringLiteral("CCI Already Exists"), "The CCI " + PrintCCI(cci) + " already exists in the database.");
                return ret;
            }
        }

        q.prepare(QStringLiteral("INSERT INTO CCI (ControlId, cci, definition) VALUES(:ControlId, :CCI, :definition)"));
        q.bindValue(QStringLiteral(":ControlId"), cci.controlId);
        q.bindValue(QStringLiteral(":CCI"), cci.cci);
        q.bindValue(QStringLiteral(":definition"), cci.definition);
        ret = q.exec();
        if (!_delayCommit)
        {
            db.commit();
            cci.id = q.lastInsertId().toInt();
        }
    }
    return ret;
}

/*!
 * \brief DbManager::AddControl
 * \param control
 * \param title
 * \param description
 * \return \c True when the \a Control is added to the database,
 * \c false when the \a Control is already part of the database or
 * has not been added.
 *
 * When providing controls formatted as FAMILY-NUMBER (ENHANCEMENT),
 * this function parses the Family, Control Number, and Enhancement
 * Number out of the string before adding it to the database. This is
 * useful when receiving new \a Controls formatted in human-readable
 * format from an external data source.
 */
bool DbManager::AddControl(const QString &control, const QString &title, const QString &description)
{
    bool ret = false;

    QString tmpControl(control.trimmed());
    if (tmpControl.length() < 4)
    {
        //control length can't store the family an a control number.
        Warning(QStringLiteral("Control Does Not Exist"), "Received bad control, \"" + control + "\".");
        return ret;
    }

    //see if there are spaces
    int tmpIndex = tmpControl.indexOf(' ');
    if (tmpIndex > 0)
    {
        //see if there's a second space
        tmpIndex = tmpControl.indexOf(' ', tmpIndex+1);
        if (tmpIndex > 0)
        {
            tmpControl = tmpControl.left(tmpIndex+1).trimmed();
        }
    }

    QString family(tmpControl.left(2));
    tmpControl = tmpControl.right(tmpControl.length()-3);
    QString enhancement = QString();
    if (tmpControl.contains('('))
    {
        //Attempt to parse the parenthesised portion of the control as an enhancement
        int tmpIndex = tmpControl.indexOf('(');
        enhancement = tmpControl.right(tmpControl.length() - tmpIndex - 1);
        enhancement = enhancement.left(enhancement.length() - 1);
        tmpControl = tmpControl.left(tmpControl.indexOf('('));
        //if it's not an integral, remove the enhancement
        if (enhancement.toInt() == 0)
            enhancement = QString();
    }

    //The family should already be in the database.
    Family f = GetFamily(family);

    if (f.id >= 0)
    {
        QSqlDatabase db;
        if (this->CheckDatabase(db))
        {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("INSERT INTO Control (FamilyId, number, enhancement, title, description) VALUES(:FamilyId, :number, :enhancement, :title, :description)"));
            q.bindValue(QStringLiteral(":FamilyId"), f.id);
            q.bindValue(QStringLiteral(":number"), tmpControl.toInt());
            q.bindValue(QStringLiteral(":enhancement"), enhancement.isEmpty() ? QVariant(QVariant::Int) : enhancement.toInt());
            q.bindValue(QStringLiteral(":title"), title);
            q.bindValue(QStringLiteral(":description"), description);
            ret = q.exec();
            if (!_delayCommit)
                db.commit();
        }
    }
    else
    {
        //Family was not found in the database.
        Warning(QStringLiteral("Family Does Not Exist"), "The Family " + family + " does not exist in the database.");
    }

    return ret;
}

/*!
 * \brief DbManager::AddFamily
 * \param acronym
 * \param description
 * \return \c True when the \a Family is added to the database,
 * \c false when the \a Family is already part of the database or
 * has not been added.
 *
 * When parsing \a Families, the standard Acronym (which becomes
 * incorporated into the \a Control's human-readable presentation)
 * corresponds to a particular \a Family. The NIST 800-53rev4
 * \a Families are (obtained from
 * \l {https://nvd.nist.gov/800-53/Rev4} {NIST}.):
 * \list
 * \li AC - Access Control
 * \li AU - Audit and Accountability
 * \li AT - Awareness and Training
 * \li CM - Configuration Management
 * \li CP - Contingency Planning
 * \li IA - Identification and Authentication
 * \li IR - Incident Response
 * \li MA - Maintenance
 * \li MP - Media Protection
 * \li PS - Personnel Security
 * \li PE - Physical and Environmental Protection
 * \li PL - Planning
 * \li PM - Program Management
 * \li RA - Risk Assessment
 * \li CA - Security Assessment and Authorization
 * \li SC - System and Communications Protection
 * \li SI - System and Information Integrity
 * \li SA - System and Services Acquisition
 */
bool DbManager::AddFamily(const QString &acronym, const QString &description)
{
    QSqlDatabase db;
    bool ret = false;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT INTO Family (Acronym, Description) VALUES(:acronym, :description)"));
        q.bindValue(QStringLiteral(":acronym"), acronym);
        q.bindValue(QStringLiteral(":description"), Sanitize(description));
        ret = q.exec();
        if (!_delayCommit)
            db.commit();
    }
    return ret;
}

/*!
 * \brief DbManager::AddSTIG
 * \param stig
 * \param checks
 * \return \c True when the \a STIG and its \a STIGChecks are added
 * to the database, \c false when the any part of the data have not
 * been added.
 *
 * When \a stigExists is \c true, the \a STIGChecks are added to the
 * existing \a STIG already in the database. Otherwise, if the
 * \a STIG already exists, the \a STIGChecks are not added.
 */
bool DbManager::AddSTIG(STIG stig, QList<STIGCheck> checks, bool stigExists)
{
    QSqlDatabase db;
    bool ret = false;
    bool stigCheckRet = true; //turns "false" if a check fails to be added
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        if (stig.id <= 0)
        {
            STIG tmpSTIG = GetSTIG(stig.title, stig.version, stig.release);
            if (tmpSTIG.id > 0)
            {
                if (stigExists)
                {
                    stig = tmpSTIG;
                }
                else
                {
                    Warning(QStringLiteral("STIG Already Exists"), "The STIG " + PrintSTIG(stig) + " already exists in the database.");
                    return ret;
                }
            }
            else
            {
                q.prepare(QStringLiteral("INSERT INTO STIG (title, description, release, version, benchmarkId, fileName) VALUES(:title, :description, :release, :version, :benchmarkId, :fileName)"));
                q.bindValue(QStringLiteral(":title"), stig.title);
                q.bindValue(QStringLiteral(":description"), stig.description);
                q.bindValue(QStringLiteral(":release"), stig.release);
                q.bindValue(QStringLiteral(":version"), stig.version);
                q.bindValue(QStringLiteral(":benchmarkId"), stig.benchmarkId);
                q.bindValue(QStringLiteral(":fileName"), stig.fileName);
                ret = q.exec();
                stig.id = q.lastInsertId().toInt();
                //do not delay this commit; the STIG should be added to the DB to prevent inconsistencies with adding the checks.
                db.commit();
            }
        }
        if (stig.id <= 0)
        {
            Warning(QStringLiteral("Unable to Add STIG"), "The new STIG, " + PrintSTIG(stig) + ", could not be added to the database.");
            return ret;
        }
        ret = true; // we have a valid STIG
        bool newChecks = false;
        //store the old value of the "delay commit" feature. The STIGCheck additions will always be a delayed commit.
        bool delayed = _delayCommit;
        if (!delayed)
            this->DelayCommit(true);

        foreach(STIGCheck c, checks)
        {
            newChecks = true;
            q.prepare(QStringLiteral("INSERT INTO STIGCheck (`STIGId`, `CCIId`, `rule`, `vulnNum`, `groupTitle`, `ruleVersion`, `severity`, `weight`, `title`, `vulnDiscussion`, `falsePositives`, `falseNegatives`, `fix`, `check`, `documentable`, `mitigations`, `severityOverrideGuidance`, `checkContentRef`, `potentialImpact`, `thirdPartyTools`, `mitigationControl`, `responsibility`, `IAControls`, `targetKey`) VALUES(:STIGId, :CCIId, :rule, :vulnNum, :groupTitle, :ruleVersion, :severity, :weight, :title, :vulnDiscussion, :falsePositives, :falseNegatives, :fix, :check, :documentable, :mitigations, :severityOverrideGuidance, :checkContentRef, :potentialImpact, :thirdPartyTools, :mitigationControl, :responsibility, :IAControls, :targetKey)"));
            q.bindValue(QStringLiteral(":STIGId"), stig.id);
            q.bindValue(QStringLiteral(":CCIId"), c.cciId);
            q.bindValue(QStringLiteral(":rule"), c.rule);
            q.bindValue(QStringLiteral(":vulnNum"), c.vulnNum);
            q.bindValue(QStringLiteral(":groupTitle"), c.groupTitle);
            q.bindValue(QStringLiteral(":ruleVersion"), c.ruleVersion);
            q.bindValue(QStringLiteral(":severity"), c.severity);
            q.bindValue(QStringLiteral(":weight"), c.weight);
            q.bindValue(QStringLiteral(":title"), c.title);
            q.bindValue(QStringLiteral(":vulnDiscussion"), c.vulnDiscussion);
            q.bindValue(QStringLiteral(":falsePositives"), c.falsePositives);
            q.bindValue(QStringLiteral(":falseNegatives"), c.falseNegatives);
            q.bindValue(QStringLiteral(":fix"), c.fix);
            q.bindValue(QStringLiteral(":check"), c.check);
            q.bindValue(QStringLiteral(":documentable"), c.documentable ? 1 : 0);
            q.bindValue(QStringLiteral(":mitigations"), c.mitigations);
            q.bindValue(QStringLiteral(":severityOverrideGuidance"), c.severityOverrideGuidance);
            q.bindValue(QStringLiteral(":checkContentRef"), c.checkContentRef);
            q.bindValue(QStringLiteral(":potentialImpact"), c.potentialImpact);
            q.bindValue(QStringLiteral(":thirdPartyTools"), c.thirdPartyTools);
            q.bindValue(QStringLiteral(":mitigationControl"), c.mitigationControl);
            q.bindValue(QStringLiteral(":responsibility"), c.responsibility);
            q.bindValue(QStringLiteral(":IAControls"), c.iaControls);
            q.bindValue(QStringLiteral(":targetKey"), c.targetKey);
            bool tmpRet = q.exec();
            stigCheckRet = stigCheckRet && tmpRet;
            if (!tmpRet)
            {
                //for every check that can't be added, pop a warning.
                Warning(QStringLiteral("Unable to Add STIGCheck"), "The STIGCheck " + PrintSTIGCheck(c) + " could not be added to STIG " + PrintSTIG(stig) + ".");
            }
        }
        //restore the old value of the "delayed commit" feature
        if (!delayed)
        {
            this->DelayCommit(false);
        }
        if (newChecks)
            db.commit();
    }
    return ret && stigCheckRet;
}

/*!
 * \brief DbManager::AddSTIGToAsset
 * \param stig
 * \param asset
 * \return \c True when the \a STIG is mapped to the \a Asset.
 * Otherwise, \c false.
 *
 * When a \a STIG is mapped to an \a Asset, a new STIG Checklist
 * is created for the Asset, and all of the \a STIG's \a STIGChecks
 * are added to the \a CKLCheck with a default status of
 * \a Status.NotChecked.
 */
bool DbManager::AddSTIGToAsset(const STIG &stig, const Asset &asset)
{
    QSqlDatabase db;
    bool ret = false;
    if (this->CheckDatabase(db))
    {
        //check if Asset and STIG exist
        Asset tmpAsset = GetAsset(asset);
        STIG tmpSTIG = GetSTIG(stig);

        //if so, attempt to add the relationship to the DB
        if (tmpAsset.id > 0 && tmpSTIG.id > 0)
        {
            QSqlQuery q(db);
                q.prepare(QStringLiteral("INSERT INTO AssetSTIG (`AssetId`, `STIGId`) VALUES(:AssetId, :STIGId)"));
                q.bindValue(QStringLiteral(":AssetId"), tmpAsset.id);
                q.bindValue(QStringLiteral(":STIGId"), tmpSTIG.id);
                ret = q.exec();
                if (ret)
                {
                    q.prepare(QStringLiteral("INSERT INTO CKLCheck (AssetId, STIGCheckId, status, findingDetails, comments, severityOverride, severityJustification) SELECT :AssetId, id, :status, '', '', '', '' FROM STIGCheck WHERE STIGId = :STIGId"));
                    q.bindValue(QStringLiteral(":AssetId"), tmpAsset.id);
                    q.bindValue(QStringLiteral(":status"), Status::NotReviewed);
                    q.bindValue(QStringLiteral(":STIGId"), tmpSTIG.id);
                    ret = q.exec();
                    db.commit();
                }
        }
    }
    return ret;
}

/*!
 * \override DbManager::DeleteAsset(Asset)
 * \brief DbManager::DeleteAsset
 * \param id
 * \return \c True when the supplied \a Asset with the supplied \a id
 * is removed from the database. Otherwise, \c false.
 */
bool DbManager::DeleteAsset(int id)
{
    return DeleteAsset(GetAsset(id));
}

/*!
 * \brief DbManager::DeleteAsset
 * \param asset
 * \return \c True when the supplied \a Asset with the supplied \a id
 * is removed from the database. Otherwise, \c false.
 */
bool DbManager::DeleteAsset(const Asset &asset)
{
    bool ret = false;
    if (asset.STIGs().count() > 0)
    {
        Warning(QStringLiteral("Asset Has Mapped STIGs"), "The Asset '" + PrintAsset(asset) + "' has STIGs selected that must be removed.");
        return ret;
    }
    QSqlDatabase db;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM Asset WHERE id = :AssetId"));
        q.bindValue(QStringLiteral(":AssetId"), asset.id);
        ret = q.exec();
        if (!_delayCommit)
            db.commit();
    }
    return ret;
}

/*!
 * \brief DbManager::DeleteCCIs
 * \return \c True when the CCIs and controls are cleared from the
 * database. Otherwise, \c false.
 *
 * Removes RMF \a Controls and \a CCIs from the database.
 */
bool DbManager::DeleteCCIs()
{
    QSqlDatabase db;
    bool ret = false;
    if (this->CheckDatabase(db))
    {
        ret = true; //assume success until one of the queries fails.
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM Family"));
        ret = q.exec() && ret; //q.exec() first to avoid short-circuit evaluation
        q.prepare(QStringLiteral("DELETE FROM Control"));
        ret = q.exec() && ret;
        q.prepare(QStringLiteral("DELETE FROM CCI"));
        ret = q.exec() && ret;
        if (!_delayCommit)
            db.commit();
    }
    return ret;
}

/*!
 * \brief DbManager::DeleteSTIG
 * \param id
 * \return \c True when the STIG identified by the provided ID is
 * deleted from the database. Otherwise, \c false.
 */
bool DbManager::DeleteSTIG(int id)
{
    QSqlDatabase db;
    bool ret = false;
    if (this->CheckDatabase(db))
    {
        //check if this STIG is used by any Assets
        STIG tmpStig = GetSTIG(id);
        QList<Asset> assets = tmpStig.Assets();
        int tmpCount = assets.count();
        if (tmpCount > 0)
        {
            QString tmpAssetStr = QString();
            foreach (const Asset &a, assets)
            {
                tmpAssetStr.append(" '" + PrintAsset(a) + "'");
            }
            Warning(QStringLiteral("STIG In Use"), "The Asset" + Pluralize(tmpCount) + tmpAssetStr + " " + Pluralize(tmpCount, "are", "is") + " currently using the selected STIG.");
            return ret;
        }
        QSqlQuery q(db);
        ret = true; //assume success from here.
        q.prepare(QStringLiteral("DELETE FROM STIGCheck WHERE STIGId = :STIGId"));
        q.bindValue(QStringLiteral(":STIGId"), id);
        ret = q.exec() && ret; //q.exec() first toavoid short-circuit evaluation
        q.prepare(QStringLiteral("DELETE FROM STIG WHERE id = :id"));
        q.bindValue(QStringLiteral(":id"), id);
        ret = q.exec() && ret;
        if (!_delayCommit)
            db.commit();
    }
    return ret;
}

/*!
 * \override DbManager::DeleteSTIG(int id)
 * \brief DbManager::DeleteSTIG
 * \param stig
 * \return \c True when the supplied \a STIG is removed rom the
 * database. Otherwise, \c false.
 */
bool DbManager::DeleteSTIG(const STIG &stig)
{
    return DeleteSTIG(stig.id);
}

/*!
 * \brief DbManager::DeleteSTIGFromAsset
 * \param stig
 * \param asset
 * \return \c True when the \a STIG has been disassociated with the
 * \a Asset in the database. Otherwise, \c false.
 */
bool DbManager::DeleteSTIGFromAsset(const STIG &stig, const Asset &asset)
{
    QSqlDatabase db;
    bool ret = false;
    if (this->CheckDatabase(db))
    {
        //make sure the STIG and Asset exist in th database
        STIG tmpSTIG = GetSTIG(stig);
        Asset tmpAsset = GetAsset(asset);

        if (tmpSTIG.id > 0 && tmpAsset.id > 0)
        {
            QSqlQuery q(db);
            ret = true; //assume success from this point
            q.prepare(QStringLiteral("DELETE FROM AssetSTIG WHERE AssetId = :AssetId AND STIGId = :STIGId"));
            q.bindValue(QStringLiteral(":AssetId"), tmpAsset.id);
            q.bindValue(QStringLiteral(":STIGId"), tmpSTIG.id);
            ret = q.exec() && ret; //q.exec() first to avoid short-circuit execution
            q.prepare(QStringLiteral("DELETE FROM CKLCheck WHERE AssetId = :AssetId AND STIGCheckId IN (SELECT id FROM STIGCheck WHERE STIGId = :STIGId)"));
            q.bindValue(QStringLiteral(":AssetId"), tmpAsset.id);
            q.bindValue(QStringLiteral(":STIGId"), tmpSTIG.id);
            ret = q.exec() && ret;
            db.commit();
        }
    }
    return ret;
}

/*!
 * \brief DbManager::GetAsset
 * \param hostName
 * \return The \a Asset object associated with the supplied
 * \a hostName. If the hostname does not exist, the \a Asset that is
 * returned is the default empty one with an ID of -1.
 */
Asset DbManager::GetAsset(const QString &hostName)
{
    //fail quietly
    QList<Asset> tmp = GetAssets(QStringLiteral("WHERE hostName = :hostName"), {std::make_tuple<QString, QVariant>(QStringLiteral(":hostName"), hostName)});
    if (tmp.count() > 0)
        return tmp.first();
    Asset a;
    return a;
}

/*!
 * \brief DbManager::GetAsset
 * \param asset
 * \return The \a Asset object associated with the supplied \a Asset
 * \a id or \a hostName. If the id does not exist in the database,
 * the \a hostName is used. The \a Asset that is returned when
 * neither the \a id nor the \a hostName is in the database is the
 * default empty one with an ID of -1.
 */
Asset DbManager::GetAsset(const Asset &asset)
{
    //first try to find the Asset by ID
    if (asset.id > 0)
    {
        Asset tmpAsset = GetAsset(asset.id);
        if (tmpAsset.id > 0)
            return tmpAsset;
    }
    //can't find by ID, findby hostName
    return GetAsset(asset.hostName);
}

/*!
 * \brief DbManager::GetAsset
 * \param id
 * \return The \a Asset object associated with the supplied \a id.
 * If the \a id does not exist, the \a Asset that is returned is the
 * default empty one with an ID of -1.
 */
Asset DbManager::GetAsset(int id)
{
    QList<Asset> tmp = GetAssets(QStringLiteral("WHERE id = :id"), {std::make_tuple<QString, QVariant>(":id", id)});
    if (tmp.count() > 0)
        return tmp.first();
    QMessageBox::warning(nullptr, QStringLiteral("Unable to Find Asset"), "The Asset ID " + QString::number(id) + " was not found in the database.");
    Asset a;
    return a;
}

/*!
 * \brief DbManager::GetAssets
 * \param whereClause
 * \param variables
 * \return A QList of \a Assets that are in the database. SQL
 * commands are dynamically built from an optional supplied
 * \a whereClause. SQL parameters are bound by supplying them in a
 * list of tuples in the \a variables parameter.
 *
 * \example GetAssets
 * \title default
 *
 * The default GetAssets() with no parameters returns all Assets in
 * the database.
 *
 * \code
 * DbManager db;
 * QList<Asset> assets = db.GetAssets();
 * \endcode
 *
 * \example GetAssets(whereClause, variabes)
 * \title where clause
 *
 * A WHERE clause with parameterized SQL can be added to the query.
 *
 * \code
 * DbManager db;
 * int id = 4; //Asset ID 4 in the database
 * QString sampleHost = "Sample";
 * //get Asset by ID
 * Asset asset = GetAssets("WHERE id = :id"),
 *                         {std::make_tuple<QString, QVariant>(QStringLiteral(":id"), id)});
 *
 * //get Asset by HostName
 * asset = GetAssets("WHERE hostName = :hostName"),
 *                    {std::make_tuple<QString, QVariant>(QStringLiteral(":hostName"), sampleHost)});
 *
 * //get Asset by ID and HostName
 * asset = GetAssets("WHERE id = :id AND hostName = :hostName"),
 *                   {
 *                       std::make_tuple<QString, QVariant>(QStringLiteral(":id"), id),
 *                       std::make_tuple<QString, QVariant>(QStringLiteral(":hostName"), sampleHost)
 *                   });
 * \endcode
 */
QList<Asset> DbManager::GetAssets(const QString &whereClause, const QList<std::tuple<QString, QVariant>> &variables)
{
    QSqlDatabase db;
    QList<Asset> ret;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        QString toPrep = QStringLiteral("SELECT Asset.`id`, Asset.`assetType`, Asset.`hostName`, Asset.`hostIP`, Asset.`hostMAC`, Asset.`hostFQDN`, Asset.`techArea`, Asset.`targetKey`, Asset.`webOrDatabase`, Asset.`webDBSite`, Asset.`webDBInstance`");
        toPrep.append(" FROM Asset");
        if (!whereClause.isNull() && !whereClause.isEmpty())
            toPrep.append(" " + whereClause);
        toPrep.append(QStringLiteral(" ORDER BY LOWER(hostName), hostName"));
        q.prepare(toPrep);
        qDebug() << toPrep;
        for (const auto &variable : variables)
        {
            QString key;
            QVariant val;
            std::tie(key, val) = variable;
            q.bindValue(key, val);
        }
        q.exec();
        while (q.next())
        {
            Asset a;
            a.id = q.value(0).toInt();
            a.assetType = q.value(1).toString();
            a.hostName = q.value(2).toString();
            a.hostIP = q.value(3).toString();
            a.hostMAC = q.value(4).toString();
            a.hostFQDN = q.value(5).toString();
            a.techArea = q.value(6).toString();
            a.targetKey = q.value(7).toString();
            a.webOrDB = q.value(8).toBool();
            a.webDbSite = q.value(9).toString();
            a.webDbInstance = q.value(10).toString();
            ret.append(a);
        }
    }
    return ret;
}

/*!
 * \overload DbManager::GetAssets
 * \brief DbManager::GetAssets
 * \param stig
 * \return A QList of \a Assets that are associated with the supplied
 * \a STIG.
 */
QList<Asset> DbManager::GetAssets(const STIG &stig)
{
    return GetAssets("JOIN AssetSTIG ON AssetSTIG.AssetId = Asset.id JOIN STIG ON STIG.id = AssetSTIG.STIGId WHERE STIG.id = :id", {std::make_tuple<QString, QVariant>(QStringLiteral(":id"), stig.id)});
}

CCI DbManager::GetCCI(int id)
{
    QList<CCI> ccis = GetCCIs(QStringLiteral("WHERE id = :id"), {std::make_tuple<QString, QVariant>(QStringLiteral(":id"), id)});
    if (ccis.count() > 0)
        return ccis.first();
    CCI ret;
    return ret;
}

/*!
 * \brief DbManager::GetCCIByCCI
 * \param cci
 * \param stig
 * \return The \a CCI in the database that corresponds to the
 * supplied \a cci.
 *
 * The \a stig parameter is optional, but it is useful for generating
 * error messages when a CCI is requested by the selected STIG.
 *
 * When the \a cci does not exist in the database, an error message
 * is opened displaying the broken \a cci information. This function
 * is typically called by STIG import routines, and the failure
 * scenario is most often triggered by STIGs not mapped to CCIs that
 * are part of the latest NIST 800-53rev4. Some STIG checks were
 * errantly "mapped" by DISA to CCIs that were removed or replaced.
 *
 * Current standard practice is to temporarily remap these to CCI-366
 * until a better mapping can be found.
 *
 * When the explicitly requested CCI or assumed default CCI-366 are
 * not in the database, the default \a CCI with ID -1 is returned.
 */
CCI DbManager::GetCCIByCCI(int cci, const STIG *stig)
{
    QList<CCI> tmpList = GetCCIs(QStringLiteral("WHERE cci = :cci"), {std::make_tuple<QString, QVariant>(QStringLiteral(":cci"), cci)});
    if (tmpList.count() > 0)
        return tmpList.first();
    QString tmpMessage = stig ? PrintSTIG(*stig) : QStringLiteral("&lt;insert%20STIG%20information%20here&gt;");
    QString cciStr = PrintCCI(cci);

    //The CCI could not be found. Assume that this will be remapped to CCI-366.
    Warning(QStringLiteral("Broken CCI"), "The CCI " + cciStr + " does not exist in NIST 800-53r4. If you are importing a STIG, please file a bug with the STIG author (probably DISA, disa.stig_spt@mail.mil) and let them know that their CCI mapping for the STIG you are trying to import is broken. For now, this broken STIG check is being remapped to CCI-000366. <a href=\"mailto:disa.stig_spt@mail.mil?subject=Incorrectly%20Mapped%20STIG%20Check&body=DISA,%0d" + tmpMessage + "%20contains%20rule(s)%20mapped%20against%20" + cciStr + "%20which%20does%20not%20exist%20in%20the%20current%20version%20of%20NIST%20800-53r4.\">Click here</a> to file this bug with DISA automatically.");
    tmpList = GetCCIs(QStringLiteral("WHERE cci = :cci"), {std::make_tuple<QString, QVariant>(QStringLiteral(":cci"), 366)});
    if (tmpList.count() > 0)
        return tmpList.first();

    //If CCI-366 isn't in the database, provide unsuccessful default CCI.
    CCI ret;
    return ret;
}

/*!
 * \overload GetCCIByCCI
 * \brief DbManager::GetCCIByCCI
 * \param cci
 * \param stig
 * \return The \a CCI identified by the ID of the supplied \a cci.
 * If the \a cci.id is not valid, the actual \a cci.cci number is
 * used.
 */
CCI DbManager::GetCCIByCCI(const CCI &cci, const STIG *stig)
{
    if (cci.id < 0)
    {
        return GetCCIByCCI(cci.cci, stig);
    }
    return GetCCI(cci.id);
}

QList<CCI> DbManager::GetCCIs(const QString &whereClause, const QList<std::tuple<QString, QVariant>> &variables)
{
    QSqlDatabase db;
    QList<CCI> ret;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        QString toPrep = QStringLiteral("SELECT id, ControlId, cci, definition, isImport, importCompliance, importDateTested, importTestedBy, importTestResults FROM CCI");
        if (!whereClause.isNull() && !whereClause.isEmpty())
            toPrep.append(" " + whereClause);
        toPrep.append(QStringLiteral(" ORDER BY cci"));
        q.prepare(toPrep);
        for (const auto &variable : variables)
        {
            QString key;
            QVariant val;
            std::tie(key, val) = variable;
            q.bindValue(key, val);
        }
        q.exec();
        while (q.next())
        {
            CCI c;
            c.id = q.value(0).toInt();
            c.controlId = q.value(1).toInt();
            c.cci = q.value(2).toInt();
            c.definition = q.value(3).toString();
            c.isImport = q.value(4).toBool();
            c.importCompliance = q.value(5).toString();
            c.importDateTested = q.value(6).toString();
            c.importTestedBy = q.value(7).toString();
            c.importTestResults = q.value(8).toString();

            ret.append(c);
        }
    }
    return ret;
}

CKLCheck DbManager::GetCKLCheck(int id)
{
    QList<CKLCheck> tmp = GetCKLChecks(QStringLiteral("WHERE id = :id"), {std::make_tuple<QString, QVariant>(QStringLiteral(":id"), id)});
    if (tmp.count() > 0)
    {
        return tmp.first();
    }
    CKLCheck ret;
    QMessageBox::warning(nullptr, QStringLiteral("Unable to Find CKLCheck"), "The CKLCheck of ID " + QString::number(id) + " was not found in the database.");
    return ret;
}

CKLCheck DbManager::GetCKLCheck(const CKLCheck &ckl)
{
    QList<CKLCheck> tmp;
    if (ckl.id <= 0)
    {
        tmp = GetCKLChecks(QStringLiteral("WHERE AssetId = :AssetId AND STIGCheckId = :STIGCheckId"),
            {std::make_tuple<QString, QVariant>(QStringLiteral(":AssetId"), ckl.assetId),
             std::make_tuple<QString, QVariant>(QStringLiteral(":STIGCheckId"), ckl.stigCheckId)});
    }
    else
    {
        tmp = GetCKLChecks(QStringLiteral("WHERE id = :id"), {std::make_tuple<QString, QVariant>(QStringLiteral(":id"), ckl.id)});
    }
    if (tmp.count() > 0)
    {
        return tmp.first();
    }
    CKLCheck ret;
    QMessageBox::warning(nullptr, QStringLiteral("Unable to Find CKLCheck"), "The CKLCheck of ID " + QString::number(ckl.id) + " (asset " + QString::number(ckl.assetId) + ", " + QString::number(ckl.stigCheckId) + ") was not found in the database.");
    return ret;
}

QList<CKLCheck> DbManager::GetCKLChecks(const Asset &asset, const STIG *stig)
{
    QString whereClause = QStringLiteral("WHERE AssetId = :AssetId");
    QList<std::tuple<QString, QVariant> > variables = {std::make_tuple<QString, QVariant>(QStringLiteral(":AssetId"), asset.id)};
    if (stig != nullptr)
    {
        whereClause.append(QStringLiteral(" AND STIGCheckId IN (SELECT id FROM STIGCheck WHERE STIGId = :STIGId)"));
        variables.append(std::make_tuple<QString, QVariant>(QStringLiteral(":STIGId"), stig->id));
    }
    return GetCKLChecks(whereClause, variables);
}

QList<CKLCheck> DbManager::GetCKLChecks(const QString &whereClause, const QList<std::tuple<QString, QVariant> > &variables)
{
    QSqlDatabase db;
    QList<CKLCheck> ret;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        QString toPrep = QStringLiteral("SELECT id, AssetId, STIGCheckId, status, findingDetails, comments, severityOverride, severityJustification FROM CKLCheck");
        if (!whereClause.isNull() && !whereClause.isEmpty())
            toPrep.append(" " + whereClause);
        q.prepare(toPrep);
        for (const auto &variable : variables)
        {
            QString key;
            QVariant val;
            std::tie(key, val) = variable;
            q.bindValue(key, val);
        }
        q.exec();
        while (q.next())
        {
            CKLCheck c;
            c.id = q.value(0).toInt();
            c.assetId = q.value(1).toInt();
            c.stigCheckId = q.value(2).toInt();
            c.status = static_cast<Status>(q.value(3).toInt());
            c.findingDetails = q.value(4).toString();
            c.comments = q.value(5).toString();
            c.severityOverride = static_cast<Severity>(q.value(6).toInt());
            c.severityJustification = q.value(7).toString();

            ret.append(c);
        }
    }
    return ret;
}

STIGCheck DbManager::GetSTIGCheck(int id)
{
    QList<STIGCheck> tmp = GetSTIGChecks(QStringLiteral("WHERE id = :id"), {std::make_tuple<QString, QVariant>(QStringLiteral(":id"), id)});
    if (tmp.count() > 0)
        return tmp.first();
    STIGCheck ret;
    QMessageBox::warning(nullptr, QStringLiteral("Unable to Find STIGCheck"), "The STIGCheck of ID " + QString::number(id) + " was not found in the database.");
    return ret;
}

STIGCheck DbManager::GetSTIGCheck(const STIG &stig, const QString &rule)
{
    QList<STIGCheck> tmp = GetSTIGChecks(QStringLiteral("WHERE STIGId = :STIGId AND rule = :rule"), {
                                             std::make_tuple<QString, QVariant>(QStringLiteral(":STIGId"), stig.id),
                                             std::make_tuple<QString, QVariant>(QStringLiteral(":rule"), rule)
                                         });
    if (tmp.count() > 0)
        return tmp.first();
    STIGCheck ret;
    QMessageBox::warning(nullptr, QStringLiteral("Unable to Find STIGCheck"), "The STIGCheck " + rule + " (STIG ID " + QString::number(stig.id) + ") was not found in the database.");
    return ret;
}

QList<STIGCheck> DbManager::GetSTIGChecks(const STIG &stig)
{
    return GetSTIGChecks(QStringLiteral("WHERE STIGId = :STIGId"), {std::make_tuple<QString, QVariant>(QStringLiteral(":STIGId"), stig.id)});
}

QList<STIGCheck> DbManager::GetSTIGChecks(const QString &whereClause, const QList<std::tuple<QString, QVariant> > &variables)
{
    QSqlDatabase db;
    QList<STIGCheck> ret;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        QString toPrep = QStringLiteral("SELECT `id`, `STIGId`, `CCIId`, `rule`, `vulnNum`, `groupTitle`, `ruleVersion`, `severity`, `weight`, `title`, `vulnDiscussion`, `falsePositives`, `falseNegatives`, `fix`, `check`, `documentable`, `mitigations`, `severityOverrideGuidance`, `checkContentRef`, `potentialImpact`, `thirdPartyTools`, `mitigationControl`, `responsibility`, `IAControls`, `targetKey` FROM STIGCheck");
        if (!whereClause.isNull() && !whereClause.isEmpty())
            toPrep.append(" " + whereClause);
        q.prepare(toPrep);
        for (const auto &variable : variables)
        {
            QString key;
            QVariant val;
            std::tie(key, val) = variable;
            q.bindValue(key, val);
        }
        q.exec();
        while (q.next())
        {
            STIGCheck c;
            c.id = q.value(0).toInt();
            c.stigId = q.value(1).toInt();
            c.cciId = q.value(2).toInt();
            c.rule = q.value(3).toString();
            c.vulnNum = q.value(4).toString();
            c.groupTitle = q.value(5).toString();
            c.ruleVersion = q.value(6).toString();
            c.severity = static_cast<Severity>(q.value(7).toInt());
            c.weight = q.value(8).toDouble();
            c.title = q.value(9).toString();
            c.vulnDiscussion = q.value(10).toString();
            c.falsePositives = q.value(11).toString();
            c.falseNegatives = q.value(12).toString();
            c.fix = q.value(13).toString();
            c.check = q.value(14).toString();
            c.documentable = q.value(15).toBool();
            c.mitigations = q.value(16).toString();
            c.severityOverrideGuidance = q.value(17).toString();
            c.checkContentRef = q.value(18).toString();
            c.potentialImpact = q.value(19).toString();
            c.thirdPartyTools = q.value(20).toString();
            c.mitigationControl = q.value(21).toString();
            c.thirdPartyTools = q.value(22).toString();
            c.iaControls = q.value(23).toString();
            c.targetKey = q.value(24).toString();
            ret.append(c);
        }
    }
    return ret;
}

QList<STIG> DbManager::GetSTIGs(const Asset &asset)
{
    return GetSTIGs(QStringLiteral("WHERE `id` IN (SELECT STIGId FROM AssetSTIG WHERE AssetId = :AssetId)"), {std::make_tuple<QString, QVariant>(QStringLiteral(":AssetId"), asset.id)});
}

QList<STIG> DbManager::GetSTIGs(const QString &whereClause, const QList<std::tuple<QString, QVariant>> &variables)
{
    QSqlDatabase db;
    QList<STIG> ret;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        QString toPrep = QStringLiteral("SELECT id, title, description, release, version, benchmarkId, fileName FROM STIG");
        if (!whereClause.isNull() && !whereClause.isEmpty())
            toPrep.append(" " + whereClause);
        toPrep.append(QStringLiteral(" ORDER BY LOWER(title), title"));
        q.prepare(toPrep);
        for (const auto &variable : variables)
        {
            QString key;
            QVariant val;
            std::tie(key, val) = variable;
            q.bindValue(key, val);
        }
        q.exec();
        while (q.next())
        {
            STIG s;
            s.id = q.value(0).toInt();
            s.title = q.value(1).toString();
            s.description = q.value(2).toString();
            s.release = q.value(3).toString();
            s.version = q.value(4).toInt();
            s.benchmarkId = q.value(5).toString();
            s.fileName = q.value(6).toString();
            ret.append(s);
        }
    }
    return ret;
}

Control DbManager::GetControl(int id)
{
    Control ret;
    QSqlDatabase db;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id, FamilyId, number, enhancement, title, description FROM Control WHERE id = :id"));
        q.bindValue(QStringLiteral(":id"), id);
        q.exec();
        if (q.next())
        {
            ret.id = q.value(0).toInt();;
            ret.familyId = q.value(1).toInt();
            ret.number = q.value(2).toInt();
            ret.enhancement = q.value(3).isNull() ? -1 : q.value(3).toInt();
            ret.title = q.value(4).toString();
            ret.description = q.value(5).toString();
        }
    }
    return ret;
}

Control DbManager::GetControl(QString control)
{
    //see if there are spaces
    control = control.trimmed();
    int tmpIndex = control.indexOf(' ');
    if (tmpIndex > 0)
    {
        //see if there's a second space
        tmpIndex = control.indexOf(' ', tmpIndex+1);
        if (tmpIndex > 0)
        {
            control = control.left(tmpIndex+1).trimmed();
        }
    }
    Control ret;
    ret.id = -1;
    ret.enhancement = -1;
    ret.title = QString();
    ret.familyId = -1;
    QString family(control.left(2));
    control = control.right(control.length()-3);
    QString enhancement = QString();
    if (control.contains('('))
    {
        int tmpIndex = control.indexOf('(');
        enhancement = control.right(control.length() - tmpIndex - 1);
        enhancement = enhancement.left(enhancement.length() - 1);
        control = control.left(control.indexOf('('));
        ret.enhancement = enhancement.toInt(); //will return 0 if enhancement doesn't exist
    }
    ret.number = control.toInt();
    ret.familyId = GetFamily(family).id;
    QSqlDatabase db;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        if (ret.enhancement > 0)
            q.prepare(QStringLiteral("SELECT id, title, description FROM Control WHERE number = :number AND FamilyId = :FamilyId AND enhancement = :enhancement"));
        else
            q.prepare(QStringLiteral("SELECT id, title, description FROM Control WHERE number = :number AND FamilyId = :FamilyId"));
        q.bindValue(QStringLiteral(":number"), ret.number);
        q.bindValue(QStringLiteral(":FamilyId"), ret.familyId);
        if (ret.enhancement > 0)
            q.bindValue(QStringLiteral(":enhancement"), ret.enhancement);
        q.exec();
        if (q.next())
        {
            ret.id = q.value(0).toInt();
            ret.title = q.value(1).toString();
            ret.description = q.value(2).toString();
        }
    }
    return ret;
}

Family DbManager::GetFamily(int id)
{
    QSqlDatabase db;
    Family ret;
    ret.id = -1;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id, acronym, description FROM Family WHERE id = :id"));
        q.bindValue(QStringLiteral(":id"), id);
        q.exec();
        if (q.next())
        {
            ret.id = q.value(0).toInt();
            ret.acronym = q.value(1).toString();
            ret.description = q.value(2).toString();
        }
    }
    return ret;
}

Family DbManager::GetFamily(const QString &acronym)
{
    QSqlDatabase db;
    Family ret;
    ret.id = -1;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id, acronym, description FROM Family WHERE acronym = :acronym"));
        q.bindValue(QStringLiteral(":acronym"), acronym);
        q.exec();
        if (q.next())
        {
            ret.id = q.value(0).toInt();
            ret.acronym = q.value(1).toString();
            ret.description = q.value(2).toString();
        }
    }
    return ret;
}

QList<Family> DbManager::GetFamilies()
{
    QSqlDatabase db;
    QList<Family> ret;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id, acronym, description FROM Family"));
        q.exec();
        while (q.next())
        {
            Family f;
            f.id = q.value(0).toInt();
            f.acronym = q.value(1).toString();
            f.description = q.value(2).toString();
            ret.append(f);
        }
    }
    return ret;
}

STIG DbManager::GetSTIG(int id)
{
    QList<STIG> tmpStigs = GetSTIGs(QStringLiteral("WHERE id = :id"), {std::make_tuple<QString, QVariant>(QStringLiteral(":id"), id)});
    if (tmpStigs.count() > 0)
        return tmpStigs.first();
    STIG ret;
    Warning(QStringLiteral("Unable to Find STIG"), "The STIG of ID " + QString::number(id) + " was not found in the database.", true);
    return ret;
}

STIG DbManager::GetSTIG(const QString &title, int version, const QString &release)
{
    QList<STIG> tmpStigs = GetSTIGs(QStringLiteral("WHERE title = :title AND release = :release AND version = :version"), {
                                        std::make_tuple<QString, QVariant>(QStringLiteral(":title"), title),
                                        std::make_tuple<QString, QVariant>(QStringLiteral(":release"), release),
                                        std::make_tuple<QString, QVariant>(QStringLiteral(":version"), version)
                                    });
    if (tmpStigs.count() > 0)
        return tmpStigs.first();
    STIG ret;
    Warning(QStringLiteral("Unable to Find STIG"), "The following STIG has not been added to the master database (This is normal if you are attempting to import a new STIG that does not currently exist in the DB, and the new STIG will likely be inserted if there are no other errors.):\nTitle: " + title + "\nVersion: " + QString::number(version) + "\n" + release, true);
    return ret;
}

STIG DbManager::GetSTIG(const STIG &stig)
{
    //find STIG by ID
    if (stig.id > 0)
    {
        STIG tmpSTIG = GetSTIG(stig.id);
        if (tmpSTIG.id > 0)
            return tmpSTIG;
    }
    //if ID is not provided or isn't in DB, find by STIG parameters
    return GetSTIG(stig.title, stig.version, stig.release);
}

QString DbManager::GetVariable(const QString &name)
{
    QSqlDatabase db;
    QString ret;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT value FROM variables WHERE name = :name"));
        q.bindValue(QStringLiteral(":name"), name);
        q.exec();
        if (q.next())
        {
            ret = q.value(0).toString();
        }
    }
    return ret;
}

void DbManager::ImportCCI(const CCI &cci)
{
    QSqlDatabase db;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE CCI SET isImport = :isImport, importCompliance = :importCompliance, importDateTested = :importDateTested, importTestedBy = :importTestedBy, importTestResults = :importTestResults"));
        q.bindValue(QStringLiteral(":isImport"), cci.isImport);
        q.bindValue(QStringLiteral(":importCompliance"), cci.importCompliance);
        q.bindValue(QStringLiteral(":importDateTested"), cci.importDateTested);
        q.bindValue(QStringLiteral(":importTestedBy"), cci.importTestedBy);
        q.bindValue(QStringLiteral(":importTestResults"), cci.importTestResults);
        q.exec();
    }
}

void DbManager::UpdateCKLCheck(const CKLCheck &check)
{
    QSqlDatabase db;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        QString toPrep = QString();
        if (check.id > 0)
            toPrep = QStringLiteral("UPDATE CKLCheck SET status = :status, findingDetails = :findingDetails, comments = :comments, severityOverride = :severityOverride, severityJustification = :severityJustification WHERE id = :id");
        else
            toPrep = QStringLiteral("UPDATE CKLCheck SET status = :status, findingDetails = :findingDetails, comments = :comments, severityOverride = :severityOverride, severityJustification = :severityJustification WHERE AssetId = :AssetId AND STIGCheckId = :STIGCheckId");
        q.prepare(toPrep);
        q.bindValue(QStringLiteral(":status"), check.status);
        q.bindValue(QStringLiteral(":findingDetails"), check.findingDetails);
        q.bindValue(QStringLiteral(":comments"), check.comments);
        q.bindValue(QStringLiteral(":severityOverride"), check.severityOverride);
        q.bindValue(QStringLiteral(":severityJustification"), check.severityJustification);
        if (check.id > 0)
        {
            q.bindValue(QStringLiteral(":id"), check.id);
        }
        else
        {
            q.bindValue(QStringLiteral(":AssetId"), check.assetId);
            q.bindValue(QStringLiteral(":STIGCheckId"), check.stigCheckId);
        }
        q.exec();
    }
}

void DbManager::UpdateVariable(const QString &name, const QString &value)
{
    QSqlDatabase db;
    if (this->CheckDatabase(db))
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE variables SET value = :value WHERE name = :name"));
        q.bindValue(QStringLiteral(":value"), value);
        q.bindValue(QStringLiteral(":name"), name);
        q.exec();
    }
}

bool DbManager::CheckDatabase(QSqlDatabase &db)
{
    db = QSqlDatabase::database(QString::number(reinterpret_cast<quint64>(QThread::currentThreadId())));
    if (!db.isOpen())
        db.open();
    if (!db.isOpen())
        return false;
    return db.isValid();
}

bool DbManager::UpdateDatabaseFromVersion(int version)
{
    QSqlDatabase db;
    if (this->CheckDatabase(db))
    {
        //upgrade to version 1 of the database
        if (version <= 0)
        {
            //New database; initial setups
            QSqlQuery q(db);
            q.prepare(QStringLiteral("CREATE TABLE `Family` ( "
                        "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                        "`Acronym`	TEXT UNIQUE, "
                        "`Description`	TEXT UNIQUE"
                        ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `Control` ( "
                        "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                        "`FamilyId`	INTEGER NOT NULL, "
                        "`number`	INTEGER NOT NULL, "
                        "`enhancement`	INTEGER, "
                        "`title`	TEXT, "
                        "`description`  TEXT, "
                        "FOREIGN KEY(`FamilyID`) REFERENCES `Family`(`id`) "
                        ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `CCI` ( "
                      "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "`ControlId`	INTEGER, "
                      "`cci`    INTEGER, "
                      "`definition`	TEXT, "
                      "`isImport` INTEGER NOT NULL DEFAULT 0, "
                      "`importCompliance`	TEXT, "
                      "`importDateTested`	TEXT, "
                      "`importTestedBy`	TEXT, "
                      "`importTestResults`	TEXT, "
                      "FOREIGN KEY(`ControlId`) REFERENCES `Control`(`id`) "
                      ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `variables` ( "
                      "`name`	TEXT, "
                      "`value`	TEXT "
                      ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `STIG` ( "
                      "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "`title`	TEXT, "
                      "`description`	TEXT, "
                      "`release`	TEXT, "
                      "`version`	INTEGER, "
                      "`benchmarkId`	TEXT, "
                      "`fileName`	TEXT "
                      ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `STIGCheck` ( "
                      "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "`STIGId`	INTEGER, "
                      "`CCIId`	INTEGER, "
                      "`rule`	TEXT, "
                      "`vulnNum`    TEXT, "
                      "`groupTitle`    TEXT, "
                      "`ruleVersion`    TEXT, "
                      "`severity`	INTEGER, "
                      "`weight` REAL, "
                      "`title`	TEXT, "
                      "`vulnDiscussion`	TEXT, "
                      "`falsePositives`	TEXT, "
                      "`falseNegatives`	TEXT, "
                      "`fix`	TEXT, "
                      "`check`	TEXT, "
                      "`documentable`	INTEGER, "
                      "`mitigations`	TEXT, "
                      "`severityOverrideGuidance`	TEXT, "
                      "`checkContentRef`	TEXT, "
                      "`potentialImpact`	TEXT, "
                      "`thirdPartyTools`	TEXT, "
                      "`mitigationControl`	TEXT, "
                      "`responsibility`	TEXT, "
                      "`IAControls` TEXT, "
                      "`targetKey` TEXT, "
                      "FOREIGN KEY(`STIGId`) REFERENCES `STIG`(`id`), "
                      "FOREIGN KEY(`CCIId`) REFERENCES `CCI`(`id`) "
                      ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `Asset` ( "
                      "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "`assetType`	TEXT, "
                      "`hostName`	TEXT UNIQUE, "
                      "`hostIP`	TEXT, "
                      "`hostMAC`	TEXT, "
                      "`hostFQDN`	TEXT, "
                      "`techArea`	TEXT, "
                      "`targetKey`	TEXT, "
                      "`webOrDatabase`	INTEGER, "
                      "`webDBSite`	TEXT, "
                      "`webDBInstance`	TEXT "
                      ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `AssetSTIG` ( "
                      "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "`AssetId`	INTEGER, "
                      "`STIGId`	INTEGER, "
                      "FOREIGN KEY(`AssetId`) REFERENCES `Asset`(`id`), "
                      "FOREIGN KEY(`STIGId`) REFERENCES `STIG`(`id`) "
                      ")"));
            q.exec();
            q.prepare(QStringLiteral("CREATE TABLE `CKLCheck` ( "
                      "`id`	INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "`AssetId`	INTEGER, "
                      "`STIGCheckId`	INTEGER, "
                      "`status`	INTEGER, "
                      "`findingDetails`	TEXT, "
                      "`comments`	TEXT, "
                      "`severityOverride`	INTEGER, "
                      "`severityJustification`	TEXT, "
                      "FOREIGN KEY(`STIGCheckId`) REFERENCES `STIGCheck`(`id`), "
                      "FOREIGN KEY(`AssetId`) REFERENCES `Asset`(`id`) "
                      ")"));
            q.exec();
            q.prepare(QStringLiteral("INSERT INTO variables (name, value) VALUES(:name, :value)"));
            q.bindValue(QStringLiteral(":name"), "version");
            q.bindValue(QStringLiteral(":value"), "1");
            q.exec();

            //write changes from update
            db.commit();
        }
    }

    return EXIT_SUCCESS;
}
