#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>

#include "hardware/rtc.h"

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "pico/util/datetime.h"

#ifdef WIFI_ENABLED
#include "pico/cyw43_arch.h"
#include "ntp.h"
#endif

#include "hub75.hpp"

// for sunrise/set times, rounded
static const float latitude = 55.0f;
static const float longitude = -1.6f;

#ifdef MATRIX_2X2
const int screen_width = 64;
const int screen_height = 64;

const int max_particles = 1600;
const int max_snow_depth = 6;
const int spawn_wind_adjust = 8;
const int melt_time = 12;
#elif defined(MATRIX_2X1)
const int screen_width = 64;
const int screen_height = 32;

const int max_particles = 800;
const int max_snow_depth = 4;
const int spawn_wind_adjust = 2;
const int melt_time = 16;
#else
const int screen_width = 32;
const int screen_height = 32;

const int max_particles = 400;
const int max_snow_depth = 4;
const int spawn_wind_adjust = 2;
const int melt_time = 16;
#endif

#ifdef MATRIX_2X2
static pimoroni::Hub75 hub75(screen_width * 2, 32, nullptr);
#else
static pimoroni::Hub75 hub75(screen_width, screen_height, nullptr);
#endif

static pimoroni::PicoGraphics_PenRGB888 graphics(hub75.width, hub75.height, nullptr);

void __isr dma_complete() {
    hub75.dma_complete();
}

struct Particle
{
    int x, y;
    int x_vel, y_vel;

    int8_t size; // 0-2
    uint8_t col;
    bool dead;
};

static int wind = 0;
static int last_wind_change = 1;

static Particle snow[max_particles]{};

static int active_snow = 0;
static int spawn_timer = 0, spawn_time = 30;
static int melt_timer = 0;

static uint8_t snow_cover[screen_width * max_snow_depth]{};

static time_t last_sunrise_sunset_update = 0;

static std::tuple<time_t, time_t> calc_sunrise_sunset(time_t time, float lat, float lng, float elevation = 0.0f) {
    // https://en.wikipedia.org/wiki/Sunrise_equation

    auto deg_to_rad = [](double d) {return d * (M_PI / 180.0);};

    // calc julian day
    double julian_date = time / 86400.0 + 2440587.5;
    double julian_day = std::ceil(julian_date - (2451545.0 + 0.0009) + 69.184 / 86400.0);

    double mean_solar_time = julian_day + 0.0009 - (lng / 360.0);
    double solar_mean_anomaly = std::fmod(357.5291 + 0.98560028 * mean_solar_time, 360.0);
    double solar_mean_anomaly_rad = deg_to_rad(solar_mean_anomaly);
    double equ_of_Center = 1.9148 * std::sin(solar_mean_anomaly_rad) + 0.02 * std::sin(2.0 * solar_mean_anomaly_rad) + 0.0003 * std::sin(3.0 * solar_mean_anomaly_rad);
    double ecliptic_longitude = std::fmod(solar_mean_anomaly + equ_of_Center + 180.0 + 102.9372, 360.0);
    double ecliptic_longitude_rad = deg_to_rad(ecliptic_longitude);
    double solar_transit = 2451545.0 + mean_solar_time + 0.0053 * std::sin(solar_mean_anomaly_rad) - 0.0069 * std::sin(2.0 * ecliptic_longitude_rad);
    double sin_declination_of_sun = std::sin(ecliptic_longitude_rad) * std::sin(deg_to_rad((23.4397)));
    double cos_declination_of_sun = std::cos(std::asin(sin_declination_of_sun));
    double hour_angle = std::acos(
        (std::sin(deg_to_rad(-0.833 - 2.076 * std::sqrt(elevation) / 60.0)) - std::sin(deg_to_rad(lat)) * sin_declination_of_sun)
        / (std::cos(deg_to_rad(lat)) * cos_declination_of_sun)
    );

    double julian_rise = solar_transit - hour_angle / (M_PI * 2.0);
    double julian_set = solar_transit + hour_angle / (M_PI * 2.0);

    time_t rise_time = (julian_rise - 2440587.5) * 86400.0;
    time_t set_time = (julian_set - 2440587.5) * 86400.0;

    return {rise_time, set_time};
}

static void map_coord(int &x, int &y)
{
#ifdef MATRIX_2X2
    // remap for matrix layout
    if(y >= 32)
    {
        y = 31 - (y - 32);
        x = (63 - x) + 64;
    }
#endif
}

static void draw_mapped_circle(int x, int y, int r) {
#ifdef MATRIX_2X2
    if(y - r < 32)
        graphics.circle({x, y}, r);

    if(y + r >= 32)
    {
        // adjust coord to force map
        int mx = x, my = y + r;
        map_coord(mx, my);
        graphics.circle({mx, my + r}, r);
    }
#else
    graphics.circle({x, y}, r);
#endif
}

int main() {
    stdio_init_all();

    // init RTC
    rtc_init();
    datetime_t initTime = {0, 1, 1, 0, 0, 0, 0};
    rtc_set_datetime(&initTime);

    hub75.start(dma_complete);

#ifdef WIFI_ENABLED
    if(cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    if(cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 10000)) {
        printf("failed to connect\n");
        return 1;
    }

    printf("wifi connected\n");

    auto ntpState = ntp_init();
    if(!ntpState)
        printf("ntp init failed\n");
#endif

    std::mt19937 randomGenerator(get_rand_32());
    std::uniform_int_distribution sizeDistribution(0, 3), 
                                  velDistribution(-1024, 1024),
                                  colDistribution(0x40, 0xFF);

    int sunrise_time_mins = 8 * 60 + 16;
    int sunset_time_mins = 15 * 60 + 41;

    while (true) {
        auto start = get_absolute_time();

        // update
        int first_dead = max_particles;
        int new_active = active_snow;

        for(int i = 0; i < active_snow; i++)
        {
            auto &snowflake = snow[i];

            snowflake.x += snowflake.x_vel;
            snowflake.y += snowflake.y_vel;

            snowflake.x_vel += wind;
            snowflake.y_vel += 8; // gravity

            // dead if fell to the bottom
            if(snowflake.y >> 16 >= screen_height)
            {
                if(first_dead == max_particles)
                    first_dead = i;
                new_active--;
                snowflake.dead = true;

                int x = (snowflake.x + (1 << 15)) >> 16;
                int y = max_snow_depth - 1;

                // build snow cover
                if(x >= 0 && x < screen_width)
                {
                    const int threshold = 200;

                    while(snow_cover[x + y * screen_width] > threshold && y > 0)
                        y--;

                    int dir = wind < 0 ? -1 : 1;

                    // avoid spikes by moving back down and to the side if possible
                    if(y + 1 < max_snow_depth && x + dir > 0 && x + dir < screen_width && snow_cover[x + dir + (y + 1) * screen_width] <= threshold)
                    {
                        x += dir;
                        y++;

                        // gravity
                        while(y + 1 < max_snow_depth - 1 && snow_cover[x + (y + 1) * screen_width] <= threshold)
                            y++;
                    }

                    snow_cover[x + y * screen_width] = std::min(0xFF, snow_cover[x + y * screen_width] + snowflake.col); 
                }
            }
        }

        if(first_dead < max_particles)
        {
            // move all the "dead" patricles to the end
            std::remove_if(std::begin(snow) + first_dead, std::begin(snow) + active_snow, [](Particle & p){return p.dead;});
            active_snow = new_active;
        }

        // add new snow particle
        if(active_snow < max_particles)
            spawn_timer -= (screen_width * screen_height) / (32 * 32); // spawn faster the larger the display
        else
            spawn_timer = spawn_timer;

        if(spawn_timer <= 0)
        {
            auto &snowflake = snow[active_snow++];

            snowflake.x = std::uniform_int_distribution(std::min(0, -wind * spawn_wind_adjust), screen_width + std::max(0, -wind * spawn_wind_adjust))(randomGenerator) << 16;
            snowflake.y = -8;

            // a bit of initial movement
            snowflake.x_vel = velDistribution(randomGenerator);
            snowflake.y_vel = velDistribution(randomGenerator) + 1024; // not up

            snowflake.size = sizeDistribution(randomGenerator);
            snowflake.col = colDistribution(randomGenerator);
            snowflake.dead = false;

            spawn_timer += spawn_time;

            // adjust time
            spawn_time += std::uniform_int_distribution(-3, 3 - spawn_time / 40)(randomGenerator);
            spawn_time = std::max(1, std::min(60, spawn_time));
        }

        // adjust wind
        int wind_adj = std::uniform_int_distribution(0, 2)(randomGenerator);
        if(wind_adj == 1) // same direction as last time
            wind += last_wind_change;
        else if(wind_adj == 2) // switch direction
        {
            last_wind_change = - last_wind_change;
            wind += last_wind_change;
        }

        wind = std::min(std::max(wind, -30), 30);

        // melting
        melt_timer--;
        if(melt_timer <= 0)
        {
            for(int x = 0; x < screen_width ; x++)
            {
                for(int y = 0; y < max_snow_depth; y++)
                {
                    if(snow_cover[x + y * screen_width] > 0)
                    {
                        snow_cover[x + y * screen_width]--;
                        break;
                    }
                }
            }
            melt_timer = melt_time;
        }


        //if(f % 60 == 0)
        //    printf("%i %i %i\n", wind, spawn_time, active_snow);
        //f++;

        // drawing

        //hub75.background = pimoroni::Pixel();

        graphics.set_pen(0, 0, 0);
        graphics.clear();

        // sun/moon
        datetime_t time;
        rtc_get_datetime(&time);

        time_t cur_time;
        if(datetime_to_time(&time, &cur_time) && cur_time - last_sunrise_sunset_update > 12 * 60 * 60) {
            // update sunrise/set times every 12h
            auto times = calc_sunrise_sunset(cur_time, latitude, longitude);

            datetime_t tmp_datetime;
    
            time_to_datetime(std::get<0>(times), &tmp_datetime);
            sunrise_time_mins = tmp_datetime.hour * 60 + tmp_datetime.min;

            time_to_datetime(std::get<1>(times), &tmp_datetime);
            sunset_time_mins = tmp_datetime.hour * 60 + tmp_datetime.min;

            printf("set sunrise to %02i:%02i, sunset to %02i:%02i\n", sunrise_time_mins / 60, sunrise_time_mins % 60, tmp_datetime.hour, tmp_datetime.min);
            last_sunrise_sunset_update = cur_time;
        }

        int time_mins = time.hour * 60 + time.min; // 1440 should be enough

        int sun_radius = screen_width / 10;
        int moon_radius = screen_width / 16;
        int sun_margin = 2;

        float day_len = sunset_time_mins - sunrise_time_mins;
        float night_len = (24 * 60) - day_len;

        float sun_y, moon_y;
        if(time_mins < sunrise_time_mins) // before sunrise
            sun_y = -std::sin((time_mins + (24 * 60) - sunset_time_mins) / night_len * M_PI);
        else if(time_mins > sunset_time_mins) // after sunset
            sun_y = -std::sin((time_mins - sunset_time_mins) / night_len * M_PI);
        else
            sun_y = std::sin((time_mins - sunrise_time_mins) / day_len * M_PI);

        int y_range = screen_height - (sun_radius + sun_margin);
        moon_y = screen_height + (sun_y * y_range);
        sun_y = screen_height - (sun_y * y_range);
    
        int x = sun_radius + sun_margin;
        int y = sun_y;
        map_coord(x, y);

        graphics.set_pen(255, 255, 0);
        draw_mapped_circle(x, y, sun_radius);

        x = screen_width - (sun_radius + sun_margin);
        y = moon_y;
        map_coord(x, y);

        graphics.set_pen(100, 100, 150);
        draw_mapped_circle(x, y, moon_radius);

        // falling snow
        for(int i = 0; i < active_snow; i++)
        {
            auto &snowflake = snow[i];

            auto putPixel = [](int x, int y, uint8_t g)
            {
                if(x < 0 || y < 0 || x >= screen_width || y >= screen_height)
                    return;

                map_coord(x, y);

                graphics.set_pen(g, g, g);
                graphics.pixel({x, y});
            };

            int sx = snowflake.x >> 16;
            int sy = snowflake.y >> 16;

            switch(snowflake.size)
            {
                case 0:
                case 1:
                case 2:
                    putPixel(sx, sy, snowflake.col);
                    break;
                case 3:
                    putPixel(sx, sy, snowflake.col);
                    putPixel(sx - 1, sy, snowflake.col / 2);
                    putPixel(sx + 1, sy, snowflake.col / 2);
                    putPixel(sx, sy - 1, snowflake.col / 2);
                    putPixel(sx, sy + 1, snowflake.col / 2);
                    break;
            }
        }

        // draw snow cover
        for(int y = 0; y < max_snow_depth; y++)
        {
            for(int x = 0; x < screen_width ; x++)
            {
                int scrY = y + (screen_height - max_snow_depth);
                int scrX = x;
                map_coord(scrX, scrY);
                
                uint8_t g = snow_cover[x + y * screen_width];

                if(!g)
                    continue;

                // snow already here
                // FIXME: this is pretty nasty, especially now that we draw things other than snow
                if(((uint8_t *)graphics.frame_buffer)[(scrX + scrY * graphics.bounds.w) * 4] > g) 
                    continue;

                graphics.set_pen(g, g, g);
                graphics.pixel({scrX, scrY});
            }
        }

        hub75.update(&graphics);

#ifdef WIFI_ENABLED
        ntp_update(ntpState);
#endif

        auto end = get_absolute_time();

        const auto targetTime = 1000000 / 60;
        auto elapsed = absolute_time_diff_us(start, end);

        if(elapsed < targetTime)
            sleep_us(targetTime - elapsed);
    }
}
