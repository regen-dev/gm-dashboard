/*
 * weather.so - GM Dashboard weather plugin v2
 *
 * Fetches current conditions from Open-Meteo (free, no API key).
 * Location resolution: LAT/LON > CITY (via Nominatim) > IP geolocation.
 *
 * Config: ~/.config/gm-dashboard/weather.conf
 *   CITY=Brasilia          (geocoded via Nominatim)
 *   LAT=-15.84             (direct coordinates)
 *   LON=-48.03
 *   # empty = auto-detect via IP geolocation
 */
#include "../gm-plugin.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QByteArray>
#include <QUrl>
#include <cmath>
#include <curl/curl.h>

/* ── Statics ─────────────────────────────────────────────────── */

static double s_lat, s_lon;
static QString s_city;
static QByteArray resultBuf;
static bool s_configured;

/* ── WMO weather code table (WMO 4677) ──────────────────────── */

struct WmoEntry {
	int code;
	const char *desc;
	const char *icon;
};

static const WmoEntry wmoTable[] = {
	{  0, "Clear Sky",              "sun"          },
	{  1, "Mainly Clear",           "sun"          },
	{  2, "Partly Cloudy",          "cloud-sun"    },
	{  3, "Overcast",               "cloud"        },
	{ 45, "Fog",                    "fog"          },
	{ 48, "Depositing Rime Fog",    "fog"          },
	{ 51, "Light Drizzle",          "drizzle"      },
	{ 53, "Moderate Drizzle",       "drizzle"      },
	{ 55, "Dense Drizzle",          "drizzle"      },
	{ 56, "Light Freezing Drizzle", "sleet"        },
	{ 57, "Dense Freezing Drizzle", "sleet"        },
	{ 61, "Slight Rain",            "rain"         },
	{ 63, "Moderate Rain",          "rain"         },
	{ 65, "Heavy Rain",             "rain-heavy"   },
	{ 66, "Light Freezing Rain",    "sleet"        },
	{ 67, "Heavy Freezing Rain",    "sleet"        },
	{ 71, "Slight Snow",            "snow"         },
	{ 73, "Moderate Snow",          "snow"         },
	{ 75, "Heavy Snow",             "snow-heavy"   },
	{ 77, "Snow Grains",            "snow"         },
	{ 80, "Slight Rain Showers",    "rain"         },
	{ 81, "Moderate Rain Showers",  "rain"         },
	{ 82, "Violent Rain Showers",   "rain-heavy"   },
	{ 85, "Slight Snow Showers",    "snow"         },
	{ 86, "Heavy Snow Showers",     "snow-heavy"   },
	{ 95, "Thunderstorm",           "thunderstorm" },
	{ 96, "Thunderstorm + Slight Hail", "thunderstorm" },
	{ 99, "Thunderstorm + Heavy Hail",  "thunderstorm" },
};

static const char *wmoDesc(int code)
{
	for (const auto &e : wmoTable)
		if (e.code == code) return e.desc;
	return "Unknown";
}

static const char *wmoIcon(int code)
{
	for (const auto &e : wmoTable)
		if (e.code == code) return e.icon;
	return "question";
}

/* ── Wind direction label ────────────────────────────────────── */

static const char *windDirLabel(int deg)
{
	/* 8-point compass from degrees */
	static const char *dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	int idx = ((deg + 22) % 360) / 45;
	if (idx < 0 || idx > 7) idx = 0;
	return dirs[idx];
}

/* ── curl helper ─────────────────────────────────────────────── */

static size_t writeCb(void *p, size_t sz, size_t n, void *ud)
{
	auto *b = static_cast<QByteArray *>(ud);
	b->append(static_cast<const char *>(p), static_cast<qsizetype>(sz * n));
	return sz * n;
}

static QByteArray fetchUrl(const char *url)
{
	CURL *c = curl_easy_init();
	if (!c) return {};
	QByteArray buf;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "gm-weather/2.0");
	CURLcode res = curl_easy_perform(c);
	curl_easy_cleanup(c);
	return res == CURLE_OK ? buf : QByteArray{};
}

/* ── Geocoding: city name → lat/lon via Nominatim ────────────── */

static bool geocodeCity(const QString &city, double &lat, double &lon)
{
	QByteArray encoded = QUrl::toPercentEncoding(city);
	QByteArray url = "https://nominatim.openstreetmap.org/search?q="
		+ encoded + "&format=json&limit=1";

	QByteArray raw = fetchUrl(url.constData());
	if (raw.isEmpty()) return false;

	QJsonDocument doc = QJsonDocument::fromJson(raw);
	if (!doc.isArray() || doc.array().isEmpty()) return false;

	QJsonObject first = doc.array()[0].toObject();
	QString sLat = first["lat"].toString();
	QString sLon = first["lon"].toString();
	if (sLat.isEmpty() || sLon.isEmpty()) return false;

	bool okLat, okLon;
	lat = sLat.toDouble(&okLat);
	lon = sLon.toDouble(&okLon);
	return okLat && okLon;
}

/* ── IP geolocation fallback ─────────────────────────────────── */

static bool geolocateIp(double &lat, double &lon, QString &city)
{
	QByteArray raw = fetchUrl("http://ip-api.com/json/?fields=status,city,lat,lon");
	if (raw.isEmpty()) return false; // LCOV_EXCL_LINE — ip-api.com unreachable

	QJsonDocument doc = QJsonDocument::fromJson(raw); // LCOV_EXCL_LINE — network-dependent
	if (!doc.isObject()) return false; // LCOV_EXCL_LINE

	QJsonObject obj = doc.object(); // LCOV_EXCL_LINE
	if (obj["status"].toString() != "success") return false; // LCOV_EXCL_LINE

	lat = obj["lat"].toDouble(); // LCOV_EXCL_LINE
	lon = obj["lon"].toDouble(); // LCOV_EXCL_LINE
	city = obj["city"].toString(); // LCOV_EXCL_LINE
	return true; // LCOV_EXCL_LINE
}

/* ── Open-Meteo response parsing ─────────────────────────────── */

static QJsonObject parseOpenMeteo(const QByteArray &raw, const QString &city)
{
	QJsonDocument doc = QJsonDocument::fromJson(raw);
	if (!doc.isObject()) return {};

	QJsonObject root = doc.object();
	QJsonObject cur = root["current"].toObject();
	QJsonObject daily = root["daily"].toObject();

	if (cur.isEmpty()) return {};

	QJsonObject r;
	r["city"] = city;
	r["lat"] = root["latitude"].toDouble();
	r["lon"] = root["longitude"].toDouble();

	/* Current conditions */
	r["temp_c"] = static_cast<int>(round(cur["temperature_2m"].toDouble()));
	r["feels_like"] = static_cast<int>(round(cur["apparent_temperature"].toDouble()));
	r["humidity"] = cur["relative_humidity_2m"].toInt();

	double windSpd = cur["wind_speed_10m"].toDouble();
	r["wind_kmh"] = static_cast<int>(round(windSpd));
	int windDeg = cur["wind_direction_10m"].toInt();
	r["wind_dir"] = windDeg;
	r["wind_dir_label"] = windDirLabel(windDeg);

	r["pressure_hpa"] = static_cast<int>(round(cur["surface_pressure"].toDouble()));
	r["uv_index"] = cur["uv_index"].toDouble();
	r["visibility_m"] = static_cast<int>(round(cur["visibility"].toDouble()));

	int wCode = cur["weather_code"].toInt();
	r["weather_code"] = wCode;
	r["desc"] = wmoDesc(wCode);
	r["icon"] = wmoIcon(wCode);
	r["is_day"] = cur["is_day"].toInt() == 1;

	/* Daily arrays */
	QJsonArray timeArr = daily["time"].toArray();
	QJsonArray maxArr = daily["temperature_2m_max"].toArray();
	QJsonArray minArr = daily["temperature_2m_min"].toArray();
	QJsonArray sunriseArr = daily["sunrise"].toArray();
	QJsonArray sunsetArr = daily["sunset"].toArray();
	QJsonArray wCodeArr = daily["weather_code"].toArray();
	QJsonArray precipArr = daily["precipitation_probability_max"].toArray();

	/* Find today's index in daily arrays (past_days shifts index 0 to yesterday) */
	QString todayDate = cur["time"].toString().left(10);
	int todayIdx = 0;
	for (int i = 0; i < timeArr.size(); i++) {
		if (timeArr[i].toString() == todayDate) { todayIdx = i; break; }
	}

	/* Today's summary */
	if (todayIdx < maxArr.size())
		r["temp_max"] = static_cast<int>(round(maxArr[todayIdx].toDouble()));
	if (todayIdx < minArr.size())
		r["temp_min"] = static_cast<int>(round(minArr[todayIdx].toDouble()));
	if (todayIdx < sunriseArr.size()) {
		QString sr = sunriseArr[todayIdx].toString();
		r["sunrise"] = sr.mid(sr.indexOf('T') + 1);
	}
	if (todayIdx < sunsetArr.size()) {
		QString ss = sunsetArr[todayIdx].toString();
		r["sunset"] = ss.mid(ss.indexOf('T') + 1);
	}

	/* 7-day forecast array (from today onwards) */
	QJsonArray forecast;
	for (int i = todayIdx; i < timeArr.size(); i++) {
		QJsonObject day;
		day["date"] = timeArr[i].toString();
		int dCode = i < wCodeArr.size() ? wCodeArr[i].toInt() : 0;
		day["weather_code"] = dCode;
		day["desc"] = wmoDesc(dCode);
		day["icon"] = wmoIcon(dCode);
		if (i < maxArr.size())
			day["temp_max"] = static_cast<int>(round(maxArr[i].toDouble()));
		if (i < minArr.size())
			day["temp_min"] = static_cast<int>(round(minArr[i].toDouble()));
		if (i < precipArr.size())
			day["precip_prob"] = precipArr[i].toInt();
		forecast.append(day);
	}
	r["forecast"] = forecast;

	/* Hourly temperature data (for sparklines) */
	QJsonArray hourlyTemps = root["hourly"].toObject()["temperature_2m"].toArray();
	QJsonArray hYesterday, hToday;
	if (todayIdx > 0) {
		int start = (todayIdx - 1) * 24;
		for (int i = start; i < start + 24 && i < hourlyTemps.size(); i++)
			hYesterday.append(hourlyTemps[i].toDouble());
	}
	{
		int start = todayIdx * 24;
		for (int i = start; i < start + 24 && i < hourlyTemps.size(); i++)
			hToday.append(hourlyTemps[i].toDouble());
	}
	r["hourly_yesterday"] = hYesterday;
	r["hourly_today"] = hToday;

	return r;
}

/* ── Nominatim response parsing ──────────────────────────────── */

static bool parseNominatim(const QByteArray &raw, double &lat, double &lon)
{
	QJsonDocument doc = QJsonDocument::fromJson(raw);
	if (!doc.isArray() || doc.array().isEmpty()) return false;

	QJsonObject first = doc.array()[0].toObject();
	QString sLat = first["lat"].toString();
	QString sLon = first["lon"].toString();
	if (sLat.isEmpty() || sLon.isEmpty()) return false;

	bool okLat, okLon;
	lat = sLat.toDouble(&okLat);
	lon = sLon.toDouble(&okLon);
	return okLat && okLon;
}

/* ── IP geo response parsing ─────────────────────────────────── */

static bool parseIpGeo(const QByteArray &raw, double &lat, double &lon, QString &city)
{
	QJsonDocument doc = QJsonDocument::fromJson(raw);
	if (!doc.isObject()) return false;

	QJsonObject obj = doc.object();
	if (obj["status"].toString() != "success") return false;

	lat = obj["lat"].toDouble();
	lon = obj["lon"].toDouble();
	city = obj["city"].toString();
	return true;
}

/* ── Plugin ABI ──────────────────────────────────────────────── */

extern "C" {

struct gm_info gm_info(void)
{
	struct gm_info i = {
		GM_API_VERSION,
		"weather",
		"WEATHER",
		"2.0.0",
		"regen-dev",
		10,
		GM_CAP_NETWORK,
		"Network: Open-Meteo weather, Nominatim geocoding, IP geolocation",
	};
	return i;
}

int gm_init(const char *config_dir)
{
	s_city.clear();
	s_lat = s_lon = 0;
	s_configured = false;
	resultBuf.clear();

	QString cfgLat, cfgLon;

	QFile f(QString("%1/weather.conf").arg(config_dir));
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		while (!f.atEnd()) {
			QString line = QString::fromUtf8(f.readLine()).trimmed();
			if (line.startsWith('#') || line.isEmpty()) continue;
			int eq = line.indexOf('=');
			if (eq < 0) continue;
			QString key = line.left(eq);
			QString val = line.mid(eq + 1);
			if (key == "CITY") s_city = val;
			else if (key == "LAT") cfgLat = val;
			else if (key == "LON") cfgLon = val;
		}
	}

	/* Priority 1: direct LAT+LON */
	if (!cfgLat.isEmpty() && !cfgLon.isEmpty()) {
		bool okLat, okLon;
		double lat = cfgLat.toDouble(&okLat);
		double lon = cfgLon.toDouble(&okLon);
		if (okLat && okLon && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
			s_lat = lat;
			s_lon = lon;
			if (s_city.isEmpty()) s_city = "Custom Location";
			s_configured = true;
			return 0;
		}
	}

	/* Priority 2: CITY → geocode via Nominatim */
	if (!s_city.isEmpty()) {
		if (geocodeCity(s_city, s_lat, s_lon)) {
			s_configured = true;
			return 0;
		}
		/* Geocoding failed — network issue, but CITY is set */
		return 0;
	}

	/* Priority 3: IP geolocation */
	if (geolocateIp(s_lat, s_lon, s_city)) { // LCOV_EXCL_LINE — ip-api.com unreliable in CI
		s_configured = true; // LCOV_EXCL_LINE
	} // LCOV_EXCL_LINE

	return 0;
}

const char *gm_fetch(void)
{
	if (!s_configured) {
		resultBuf = "{\"not_configured\":true}";
		return resultBuf.constData();
	}

	QByteArray url = QString::asprintf(
		"https://api.open-meteo.com/v1/forecast?"
		"latitude=%.4f&longitude=%.4f"
		"&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
		"weather_code,wind_speed_10m,wind_direction_10m,surface_pressure,"
		"uv_index,visibility,is_day"
		"&daily=weather_code,temperature_2m_max,temperature_2m_min,"
		"precipitation_probability_max,sunrise,sunset"
		"&hourly=temperature_2m"
		"&timezone=auto&past_days=1&forecast_days=7",
		s_lat, s_lon).toUtf8();

	QByteArray raw = fetchUrl(url.constData());
	if (raw.isEmpty()) return nullptr;

	QJsonObject r = parseOpenMeteo(raw, s_city);
	if (r.isEmpty()) return nullptr;

	resultBuf = QJsonDocument(r).toJson(QJsonDocument::Compact);
	return resultBuf.constData();
}

void gm_cleanup(void)
{
	s_city.clear();
	resultBuf.clear();
	s_configured = false;
	s_lat = s_lon = 0;
}

} /* extern "C" */
