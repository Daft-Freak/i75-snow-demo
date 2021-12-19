#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>

#include "pico/stdlib.h"

//#include "common/pimoroni_common.hpp"

#include "hub75.hpp"

const int screen_width = 32;
const int screen_height = 32;

static Hub75 hub75(screen_width, screen_height, nullptr);

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

const int max_particles = 400;
static Particle snow[max_particles]{};

static int active_snow = 0;
static int spawn_timer = 0, spawn_time = 30;
static int melt_timer = 0, melt_time = 16;

const int max_snow_depth = 4;
static uint8_t snow_cover[screen_width * max_snow_depth]{};

int main() {
    hub75.start(dma_complete);

    std::mt19937 randomGenerator(0xDAF7);
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
            if(snowflake.y >> 16 >= screen_width)
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
                        while(y + 1 < max_snow_depth - 1 && !snow_cover[x + (y + 1) * screen_width] <= threshold)
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
        spawn_timer--;
        if(active_snow < max_particles && spawn_timer <= 0)
        {
            auto &snowflake = snow[active_snow++];
    
            snowflake.x = std::uniform_int_distribution(std::min(0, -wind * 2), screen_width + std::max(0, -wind * 2))(randomGenerator) << 16;
            snowflake.y = -8;

            // a bit of initial movement
            snowflake.x_vel = velDistribution(randomGenerator);
            snowflake.y_vel = velDistribution(randomGenerator) + 1024; // not up

            snowflake.size = sizeDistribution(randomGenerator);
            snowflake.col = colDistribution(randomGenerator);
            snowflake.dead = false;

            // adjust time
            spawn_time += std::uniform_int_distribution(-3, 3 - spawn_time / 40)(randomGenerator);
            spawn_time = std::max(1, std::min(60, spawn_time));

            spawn_timer = spawn_time;
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

        hub75.background = Pixel();

        for(int i = 0; i < active_snow; i++)
        {
            auto &snowflake = snow[i];

            auto putPixel = [&hub75](int x, int y, uint8_t g)
            {
                if(x < 0 || y < 0 || x >= screen_width || y >= screen_height)
                    return;

                int off = (x + y * screen_width) * 4;

                hub75.set_color(x, y, {g, g, g});
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

        for(int y = 0; y < max_snow_depth; y++)
        {
            for(int x = 0; x < screen_width ; x++)
            {
                int scrY = y + (screen_height - max_snow_depth);
                int off = (x + (scrY % (screen_height / 2)) * screen_width) * 2;

                if(scrY >= screen_height / 2)
                    off++;
                
                uint8_t g = snow_cover[x + y * screen_width];

                // snow already here
                if((hub75.front_buffer[off].color & 0x3FF) > GAMMA_10BIT[g]) 
                    continue;
    

                hub75.set_color(x, scrY, {g, g, g});
            }
        }

        hub75.flip(false);

        auto end = get_absolute_time();

        sleep_us(1000000 / 60 - absolute_time_diff_us(start, end));
    }
}
