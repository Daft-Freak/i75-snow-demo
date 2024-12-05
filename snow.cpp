#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>

#include "pico/stdlib.h"
#include "pico/rand.h"

#ifdef WIFI_ENABLED
#include "pico/cyw43_arch.h"
#include "ntp.h"
#endif

#include "hub75.hpp"

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

int main() {
    stdio_init_all();

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

                // snow already here
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
