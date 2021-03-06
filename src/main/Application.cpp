/**
 * Qt5 OpenGL video demo application
 * Copyright (C) 2018 Carlos Rafael Giani < dv AT pseudoterminal DOT org >
 *
 * qtglviddemo is free software: you can redistribute it and/or modify
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <assert.h>
#include <iostream>
#include <QFile>
#include <QDebug>
#include <QOpenGLFunctions>
#include <QLoggingCategory>
#include <QQmlContext>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QCommandLineParser>
#include "scene/GLResources.hpp"
#include "Application.hpp"


Q_DECLARE_LOGGING_CATEGORY(lcQtGLVidDemo)


namespace qtglviddemo
{


namespace
{

QString toString(VideoObjectModel::SubtitleSource p_subtitleSource)
{
	switch (p_subtitleSource)
	{
		case VideoObjectModel::SubtitleSource::FIFOSubtitles: return "fifo";
		case VideoObjectModel::SubtitleSource::MediaSubtitles: return "media";
		case VideoObjectModel::SubtitleSource::SystemStatsSubtitles: return "systemStats";
		default: assert(false);
	}

	return "";
}

bool fromString(QString const p_string, VideoObjectModel::SubtitleSource &p_subtitleSource)
{
	if      (p_string == "fifo")        p_subtitleSource = VideoObjectModel::SubtitleSource::FIFOSubtitles;
	else if (p_string == "media")       p_subtitleSource = VideoObjectModel::SubtitleSource::MediaSubtitles;
	else if (p_string == "systemStats") p_subtitleSource = VideoObjectModel::SubtitleSource::SystemStatsSubtitles;
	else return false;

	return true;
}

} // unnamed namespace end


Application::Application(int &argc, char **argv)
	: QApplication(argc, argv)
	, m_saveConfigAtEnd(false)
	, m_keepSplashscreen(false)
	, m_fullscreen(false)
	, m_renderingDuration(0)
{
	// Set some information about our application.
	QGuiApplication::setApplicationName("qtglviddemo");
	QGuiApplication::setApplicationVersion("1.0");

	// Set this application object as the context object. This way, properties
	// like keepSplashscreen can be accessed from QML, and functions marked
	// with Q_INVOKABLE can be called from QML.
	m_engine.rootContext()->setContextObject(this);
}


Application::~Application()
{
	m_fifoWatch.stop();

	if (m_saveConfigAtEnd)
		saveConfiguration();
}


bool Application::prepare()
{
	loadConfiguration();

	// Load the QML from our resources.
	m_engine.load(QUrl("qrc:/UserInterface.qml"));
	if (m_engine.rootObjects().empty())
		return false;

	// Get the main window for setting the minimum size and
	// for the getMainWindow() function.
	m_mainWindow = qobject_cast < QQuickWindow* > (m_engine.rootObjects().value(0));
	m_mainWindow->setMinimumSize(QSize(800, 600));	

	// Make sure the window is visible.
	if (m_fullscreen)
		m_mainWindow->showFullScreen();
	else
		m_mainWindow->show();

	connect(m_mainWindow, &QQuickWindow::beforeRendering, this, &Application::onBeforeRendering, Qt::DirectConnection);
	connect(m_mainWindow, &QQuickWindow::afterRendering, this, &Application::onAfterRendering, Qt::DirectConnection);

	return true;
}


std::pair < bool, int > Application::parseCommandLineArgs()
{
	QCommandLineParser cmdlineParser;
	cmdlineParser.setApplicationDescription("Qt5 OpenGL video demo");

	QCommandLineOption helpOption = cmdlineParser.addHelpOption();
	QCommandLineOption versionOption = cmdlineParser.addVersionOption();
	QCommandLineOption writeConfigAtEndOption(QStringList() << "w" << "write-config-at-end", "Write configuration when program is ended");
	cmdlineParser.addOption(writeConfigAtEndOption);
	QCommandLineOption configFileOption(QStringList() << "c" << "config-file", "Configuration file to use", "config-file");
	cmdlineParser.addOption(configFileOption);
	QCommandLineOption fullscreenOption(QStringList() << "f" << "fullscreen", "Run in fullscreen mode");
	cmdlineParser.addOption(fullscreenOption);

	if (!cmdlineParser.parse(arguments()))
	{
		std::cerr << cmdlineParser.errorText().toStdString() << "\n";
		std::cerr << "\n";
		cmdlineParser.showHelp();
		return std::make_pair(false, -1);
	}

	if (cmdlineParser.isSet(helpOption))
	{
		cmdlineParser.showHelp();
		return std::make_pair(false, 0);
	}

	if (cmdlineParser.isSet(versionOption))
	{
		cmdlineParser.showVersion();
		return std::make_pair(false, 0);
	}

	if (cmdlineParser.isSet(configFileOption))
	{
		m_configFilename = cmdlineParser.value(configFileOption);
		qCDebug(lcQtGLVidDemo) << "Using configuration filename" << m_configFilename;
	}

	if (cmdlineParser.isSet(writeConfigAtEndOption))
	{
		qCDebug(lcQtGLVidDemo) << "Will save configuration when program ends";
		m_saveConfigAtEnd = true;
	}

	m_fullscreen = cmdlineParser.isSet(fullscreenOption);

	return std::make_pair(true, 0);
}


QString Application::getSystemStats() const
{
	GstClockTime dur;

	{
		std::lock_guard < std::mutex > lock(m_sysStatsMutex);
		dur = m_renderingDuration;
	}

	m_systemStats.update();
	return QString("CPU %1%<br>memory %2% (%3 kB)<br>%4 ms render time (%5 FPS)")
	       .arg(int(m_systemStats.getNormalizedCpuUsage() * 100.0f))
	       .arg(int(m_systemStats.getNormalizedMemoryUsage() * 100.0f))
	       .arg(m_systemStats.getMemoryUsageInBytes() / 1024)
	       .arg(double(dur) / double(GST_MSECOND), 0, 'f', 2)
	       .arg(double(GST_SECOND) / double(dur), 0, 'f', 1)
	       ;
}


QQuickWindow& Application::getMainWindow()
{
	return *m_mainWindow;
}


void Application::onBeforeRendering()
{
	m_beginRenderingTimestamp = gst_util_get_timestamp();
}


void Application::onAfterRendering()
{
	std::lock_guard < std::mutex > lock(m_sysStatsMutex);
	GstClockTime afterRenderingTimestamp = gst_util_get_timestamp();
	m_renderingDuration = GST_CLOCK_DIFF(m_beginRenderingTimestamp, afterRenderingTimestamp);
}


VideoObjectModel* Application::getVideoObjectModel()
{
	return &m_videoObjectModel;
}


VideoInputDevicesModel* Application::getVideoInputDevicesModel()
{
	return &m_videoInputDevicesModel;
}


FifoWatch* Application::getFifoWatch()
{
	return &m_fifoWatch;
}


QUrl Application::getSplashscreenUrl()
{
	return QUrl::fromLocalFile(m_splashScreenFilename);
}


bool Application::getKeepSplashscreen()
{
	return m_keepSplashscreen;
}


void Application::loadConfiguration()
{
	// First, some sanity checks.

	if (m_configFilename.isEmpty())
		return;

	qCDebug(lcQtGLVidDemo) << "Loading configuration from file" << m_configFilename;

	QFile jsonFile(m_configFilename);
	if (!jsonFile.exists())
	{
		qCDebug(lcQtGLVidDemo) << "Configuration file does not exist; not parsing anything";
		return;
	}

	if (!jsonFile.open(QFile::ReadOnly))
	{
		qCWarning(lcQtGLVidDemo) << "Could not open configuration file for reading:" << jsonFile.errorString();
		return;
	}

	// Try to parse the data from the config file.
	QJsonParseError parseError;
	QJsonDocument document = QJsonDocument::fromJson(jsonFile.readAll(), &parseError);

	// If the file is empty, or has invalid JSON, exit here.
	if (document.isNull())
	{
		qCWarning(lcQtGLVidDemo) << "Could not parse configuration file: " << parseError.errorString();
		return;
	}

	QJsonObject jsonObject = document.object();

	// Iterate over each item and create a video object description out of its
	// JSON data. Then, feed this new description into the video object model.
	auto itemsJsonIter = jsonObject.find("items");
	if ((itemsJsonIter != jsonObject.end()) && itemsJsonIter->isArray())
	{
		QJsonArray itemsJsonArray = itemsJsonIter->toArray();

		for (auto arrayEntry : itemsJsonArray)
		{
			if (!arrayEntry.isObject())
			{
				qCWarning(lcQtGLVidDemo) << "Skipping non-object items array entry";
				continue;
			}

			QJsonObject itemJsonObject = arrayEntry.toObject();

			QJsonObject::iterator descIter;
			VideoObjectModel::Description desc;

			// Read the description values (if they are present in the
			// item's JSON object).

			descIter = itemJsonObject.find("url");
			if ((descIter != itemJsonObject.end()) && descIter->isString())
				desc.m_url = descIter->toString();

			descIter = itemJsonObject.find("meshType");
			if ((descIter != itemJsonObject.end()) && descIter->isString())
				desc.m_meshType = descIter->toString();

			descIter = itemJsonObject.find("scale");
			if ((descIter != itemJsonObject.end()) && descIter->isDouble())
				desc.m_scale = descIter->toDouble();

			descIter = itemJsonObject.find("rotation");
			if ((descIter != itemJsonObject.end()) && descIter->isArray())
			{
				QJsonArray rotArray = descIter->toArray();
				if (rotArray.size() >= 4)
				{
					desc.m_rotation = QQuaternion(
						rotArray[0].toDouble(),
						rotArray[1].toDouble(),
						rotArray[2].toDouble(),
						rotArray[3].toDouble()
					);
				}
			}

			descIter = itemJsonObject.find("opacity");
			if ((descIter != itemJsonObject.end()) && descIter->isDouble())
				desc.m_opacity = descIter->toDouble();

			descIter = itemJsonObject.find("cropRectangle");
			if ((descIter != itemJsonObject.end()) && descIter->isArray())
			{
				QJsonArray cropRectangleArray = descIter->toArray();
				if (cropRectangleArray.size() >= 4)
				{
					desc.m_cropRectangle = QRect(
						cropRectangleArray[0].toInt(),
						cropRectangleArray[1].toInt(),
						cropRectangleArray[2].toInt(),
						cropRectangleArray[3].toInt()
					);
				}
			}

			descIter = itemJsonObject.find("textureRotation");
			if ((descIter != itemJsonObject.end()) && descIter->isDouble())
				desc.m_textureRotation = descIter->toInt();

			descIter = itemJsonObject.find("subtitleSource");
			if ((descIter != itemJsonObject.end()) && descIter->isString())
			{
				VideoObjectModel::SubtitleSource subtitleSource;
				if (fromString(descIter->toString(), subtitleSource))
					desc.m_subtitleSource = subtitleSource;
			}

			// Add the description to the data model if it has a
			// valid URL (otherwise, no video can be played).
			if (desc.m_url.isValid())
				m_videoObjectModel.addDescription(std::move(desc));
		}
	}

	// Check if the device node name map is defined, and read it if so.
	auto deviceNodeNameMapIter = jsonObject.find("deviceNodeNameMap");
	if ((deviceNodeNameMapIter != jsonObject.end()) && deviceNodeNameMapIter->isArray())
	{
		VideoInputDevicesModel::DeviceNodeNameMap deviceNodeNameMap;
		QJsonArray deviceNodeNameArray = deviceNodeNameMapIter->toArray();

		for (auto arrayEntry : deviceNodeNameArray)
		{
			if (!arrayEntry.isObject())
			{
				qCWarning(lcQtGLVidDemo) << "Skipping non-object items array entry";
				continue;
			}

			QJsonObject deviceNodeNameObject = arrayEntry.toObject();
			QJsonObject::iterator nodeNameIter;

			QString node;
			nodeNameIter = deviceNodeNameObject.find("node");
			if (nodeNameIter != deviceNodeNameObject.end())
			{
				if (!nodeNameIter->isString())
				{
					qCWarning(lcQtGLVidDemo) << "Skipping device node name map entry with invalid node";
					continue;
				}
				node = nodeNameIter->toString();
			}

			QString name;
			nodeNameIter = deviceNodeNameObject.find("name");
			if (nodeNameIter != deviceNodeNameObject.end())
			{
				if (!nodeNameIter->isString())
				{
					qCWarning(lcQtGLVidDemo) << "Skipping device node name map entry with invalid node";
					continue;
				}
				name = nodeNameIter->toString();
			}

			qCDebug(lcQtGLVidDemo) << "Adding entry into device node name map: node:" << node << "name:" << name;
			deviceNodeNameMap.emplace(std::move(node), std::move(name));
		}

		m_videoInputDevicesModel.setDeviceNodeNameMap(std::move(deviceNodeNameMap));
	}

	// Check if a FIFO path was defined in the configuration. If so, pass
	// it to the FIFO watch and start it.
	auto fifoPathIter = jsonObject.find("fifoPath");
	if ((fifoPathIter != jsonObject.end()) && fifoPathIter->isString())
	{
		m_fifoPath = fifoPathIter->toString();
		qCDebug(lcQtGLVidDemo) << "FIFO path " << m_fifoPath << " found in configuration";

		m_fifoWatch.start(m_fifoPath, true);
	}
	else
		qCDebug(lcQtGLVidDemo) << "FIFO path not found in configuration";

	// Check splashscreen settings.
	auto splashscreenIter = jsonObject.find("splashscreen");
	if ((splashscreenIter != jsonObject.end()) && splashscreenIter->isObject())
	{
		QJsonObject splashscreenObject = splashscreenIter->toObject();

		auto splashscreenFilenameIter = splashscreenObject.find("filename");
		if ((splashscreenFilenameIter != splashscreenObject.end()) && splashscreenFilenameIter->isString())
		{
			m_splashScreenFilename = splashscreenFilenameIter->toString();
			qCDebug(lcQtGLVidDemo) << "Using splashscreen filename" << m_splashScreenFilename;
		}
		else
			qCDebug(lcQtGLVidDemo) << "No splashscreen filename was specified";

		if (!m_splashScreenFilename.isEmpty())
		{
			auto keepSplashscreenIter = splashscreenObject.find("keep");
			if ((keepSplashscreenIter != splashscreenObject.end()) && keepSplashscreenIter->isBool())
				m_keepSplashscreen = keepSplashscreenIter->toBool();

			qCDebug(lcQtGLVidDemo) << "Keeping splashscreen:" << m_keepSplashscreen;
		}
	}
}


void Application::saveConfiguration()
{
	// First, some sanity checks.

	if (m_configFilename.isEmpty())
		return;

	qCDebug(lcQtGLVidDemo) << "Saving configuration to file" << m_configFilename;

	QFile jsonFile(m_configFilename);
	if (!jsonFile.open(QFile::WriteOnly))
	{
		qCWarning(lcQtGLVidDemo) << "Could not open configuration file for writing:" << jsonFile.errorString();
		return;
	}

	QJsonObject jsonObject;

	// Serialize all video object descriptions to JSON.
	if (m_videoObjectModel.getNumDescriptions() > 0)
	{
		QJsonArray itemsJsonArray;

		for (std::size_t i = 0; i < m_videoObjectModel.getNumDescriptions(); ++i)
		{
			VideoObjectModel::Description const & desc = m_videoObjectModel.getDescription(i);
			QQuaternion const & rot = desc.m_rotation;
			QRectF const & cropRectangle = desc.m_cropRectangle;

			QJsonObject itemJsonObject;
			itemJsonObject["url"]             = desc.m_url.toString();
			itemJsonObject["meshType"]        = desc.m_meshType;
			itemJsonObject["scale"]           = desc.m_scale;
			itemJsonObject["rotation"]        = QJsonArray{rot.scalar(), rot.x(), rot.y(), rot.z()};
			itemJsonObject["opacity"]         = desc.m_opacity;
			itemJsonObject["cropRectangle"]   = QJsonArray{cropRectangle.x(), cropRectangle.y(), cropRectangle.width(), cropRectangle.height()};

			itemJsonObject["textureRotation"] = desc.m_textureRotation;
			itemJsonObject["subtitleSource"]  = toString(desc.m_subtitleSource);
			itemsJsonArray.append(itemJsonObject);
		}

		jsonObject["items"] = itemsJsonArray;
	}

	if (!m_fifoPath.isEmpty())
		jsonObject["fifoPath"] = m_fifoPath;

	if (!m_videoInputDevicesModel.getDeviceNodeNameMap().empty())
	{
		QJsonArray deviceNodeNameArray;

		for (auto const & entry : m_videoInputDevicesModel.getDeviceNodeNameMap())
		{
			QJsonObject deviceNodeNameObject;
			deviceNodeNameObject["node"] = entry.first;
			deviceNodeNameObject["name"] = entry.second;
			deviceNodeNameArray.append(deviceNodeNameObject);
		}

		jsonObject["deviceNodeNameMap"] = deviceNodeNameArray;
	}

	if (!m_splashScreenFilename.isEmpty())
	{
		QJsonObject splashscreenObject;
		splashscreenObject["filename"] = m_splashScreenFilename;
		splashscreenObject["keep"] = m_keepSplashscreen;
		jsonObject["splashscreen"] = splashscreenObject;
	}

	jsonFile.write(QJsonDocument(jsonObject).toJson());
}


} // namespace qtglviddemo end
