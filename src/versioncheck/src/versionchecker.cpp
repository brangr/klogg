/*
 * Copyright (C) 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2019 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "versionchecker.h"
#include "configuration.h"
#include "log.h"

#include "version.h"

#include <QNetworkProxyFactory>

const char* VersionChecker::VERSION_URL
    = "https://raw.githubusercontent.com/brangr/klogg/master/latest.json";

const uint64_t VersionChecker::CHECK_INTERVAL_S = 3600 * 24 * 7; /* 7 days */

#if defined( Q_OS_WIN )
#define OS_SUFFIX "-win"
#elif defined( Q_OS_OSX )
#define OS_SUFFIX "-osx"
#else
#define OS_SUFFIX "-linux"
#endif

namespace {
bool isVersionNewer( const QString& current_version, const QString& new_version )
{
    const auto parseVersion = []( const QString& version_string ) {
        int tweak_index = 0;
        auto version = QVersionNumber::fromString( version_string, &tweak_index );
        return std::make_pair( version, version_string.rightRef( tweak_index + 1 ).toUInt() );
    };

    const auto old = parseVersion( current_version );
    const auto next = parseVersion( new_version );

    return next > old;
}

} // namespace

void VersionCheckerConfig::retrieveFromStorage( QSettings& settings )
{
    LOG( logDEBUG ) << "VersionCheckerConfig::retrieveFromStorage";

    if ( settings.contains( "VersionChecker/nextDeadline" ) )
        next_deadline_ = settings.value( "VersionChecker/nextDeadline" ).toLongLong();
}

void VersionCheckerConfig::saveToStorage( QSettings& settings ) const
{
    LOG( logDEBUG ) << "VersionCheckerConfig::saveToStorage";

    settings.setValue( "VersionChecker/nextDeadline", static_cast<long long>( next_deadline_ ) );
}

VersionChecker::VersionChecker()
    : QObject()
    , manager_( new QNetworkAccessManager( this ) )
{
    QNetworkProxyFactory::setUseSystemConfiguration( true );
    manager_->setRedirectPolicy( QNetworkRequest::NoLessSafeRedirectPolicy );
}

void VersionChecker::startCheck()
{
    LOG( logDEBUG ) << "VersionChecker::startCheck()";

    const auto& deadlineConfig = VersionCheckerConfig::getSynced();
    const auto& appConfig = Configuration::get();

    if ( appConfig.versionCheckingEnabled() ) {
        // Check the deadline has been reached
        if ( deadlineConfig.nextDeadline() < std::time( nullptr ) ) {
            connect( manager_, &QNetworkAccessManager::finished, this,
                     &VersionChecker::downloadFinished );

            LOG( logDEBUG ) << "Requesting new version info from " << VERSION_URL;

            QNetworkRequest request;
            request.setUrl( QUrl( VERSION_URL ) );
            manager_->get( request );
        }
        else {
            LOG( logDEBUG ) << "Deadline not reached yet, next check in "
                            << std::difftime( deadlineConfig.nextDeadline(), std::time( nullptr ) );
        }
    }
}

void VersionChecker::downloadFinished( QNetworkReply* reply )
{
    LOG( logDEBUG ) << "VersionChecker::downloadFinished()";

    if ( reply->error() == QNetworkReply::NoError ) {
        const auto currentVersion = QString( GLOGG_VERSION );

        const auto rawReply = reply->readAll();
        LOG( logDEBUG ) << "Version reply: " << QString::fromUtf8( rawReply );

        const auto latestJson = QJsonDocument::fromJson( rawReply );
        const auto latestVersionMap = latestJson.toVariant().toMap();

        QString latestVersion;
        QString url;
        const auto stableVersions = latestVersionMap.value( "releases" ).toList();

        if ( std::any_of( stableVersions.begin(), stableVersions.end(),
                          [&currentVersion]( const auto& version ) {
                              return version.toString() == currentVersion;
                          } ) ) {
            latestVersion = latestVersionMap.value( "stable" ).toString();
            url = latestVersionMap.value( "stable_url" ).toString();
        }
        else {
            latestVersion = latestVersionMap.value( "ci" ).toString();
            url = latestVersionMap.value( "ci_url" ).toString();
        }

        LOG( logDEBUG ) << "Current version: " << currentVersion << ". Latest version is "
                        << latestVersion << ", url " << url;
        if ( isVersionNewer( currentVersion, latestVersion ) ) {
            LOG( logDEBUG ) << "Sending new version notification";
            emit newVersionFound( latestVersion, url );
        }
    }
    else {
        LOG( logWARNING ) << "Download failed: err " << reply->error();
    }

    reply->deleteLater();

    // Extend the deadline
    auto& config = VersionCheckerConfig::get();

    config.setNextDeadline( std::time( nullptr ) + CHECK_INTERVAL_S );

    config.save();
}
