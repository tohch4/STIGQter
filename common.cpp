/*
 * STIGQter - STIG fun with Qt
 *
 * Copyright © 2018 Jon Hood, http://www.hoodsecurity.com/
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

#include "common.h"
#include "tidy.h"
#include "tidybuffio.h"

#include <QEventLoop>
#include <QString>
#include <QtNetwork>
#include <iostream>

#define VERSION "0.0.1"

QString DownloadPage(QUrl u)
{
    QNetworkAccessManager manager;
    QNetworkRequest req = QNetworkRequest(QUrl(u));
    QString userAgent = QString("STIGQter/") + VERSION;
    req.setRawHeader("User-Agent", userAgent.toStdString().c_str());
    QNetworkReply *response = manager.get(req);
    QEventLoop event;
    QObject::connect(response,SIGNAL(finished()),&event,SLOT(quit()));
    event.exec();
    QString html = response->readAll();
    delete response;
    return html;
}

QString HTML2XHTML(QString s)
{
    TidyBuffer output = {nullptr};
    TidyBuffer err = {nullptr};

    int rc = -1;
    bool ok = false;

    TidyDoc tdoc = tidyCreate();
    ok = tidyOptSetBool( tdoc, TidyXmlOut, yes );
    if (ok)
        rc = tidySetErrorBuffer(tdoc, &err);
    if (rc >= 0)
        rc = tidyParseString(tdoc, s.toStdString().c_str());
    if (rc >= 0)
        rc = tidyCleanAndRepair(tdoc);
    if (rc >= 0)
        rc = tidyRunDiagnostics(tdoc);
    if (rc > 1)
        rc = (tidyOptSetBool(tdoc, TidyForceOutput, yes) ? rc : -1);
    if (rc >= 0)
        rc = tidySaveBuffer(tdoc, &output);
    if (rc >= 0 && (output.bp))
        s = QString::fromUtf8(reinterpret_cast<char*>(output.bp));
    else
        qDebug() << "A severe error (" << rc << ") occurred.";

    tidyBufFree(&output);
    tidyBufFree(&err);
    tidyRelease(tdoc);

    QString ret(s);
    ret = ret.replace("&nbsp;", " ");
    return ret;
}

bool DownloadFile(QUrl u, QFile *f)
{
    bool close = false;
    if (!f->isOpen())
    {
        f->open(QIODevice::WriteOnly);
        if (!f->isOpen())
            return false;
        close = true;
    }
    QNetworkAccessManager manager;
    QNetworkRequest req = QNetworkRequest(u);
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    QString userAgent = QString("STIGQter/") + VERSION;
    req.setRawHeader("User-Agent", userAgent.toStdString().c_str());
    QNetworkReply *response = manager.get(req);
    QEventLoop event;
    QObject::connect(response,SIGNAL(finished()),&event,SLOT(quit()));
    event.exec();
    QByteArray tmpArray = response->readAll();
    f->write(tmpArray, tmpArray.size());
    f->flush();
    delete response;

    if (close)
        f->close();
    else
        f->seek(0);
    return true;
}
