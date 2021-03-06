
#include "game.h"
#include "vecoptions.h"

void bgr32_to_rgb888(void *dst_rgb888, void *src_bgr32, int w, int h) {
    uint8_t *src = (uint8_t *)src_bgr32;
    uint8_t *dst = (uint8_t *)dst_rgb888;

    for (int y = 0; y < h; y++) {
        uint8_t *s = src + y * w * 4;
        uint8_t *d = dst + y * w * 3;
        for (int x = 0; x < w; x++) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            s += 4;
            d += 3;
        }
    }
}

Game::Game() {
    timeout = 1000;
    episodes_remaining = 0;
    last_ep_reward = 0;
    last_reward = -1;
    default_action = 0;
    fixed_asset_seed = 0;
    reset_count = 0;
    current_level_seed = 0;

    step_data.reward = 0;
    step_data.done = false;
    step_data.level_complete = false;

    register_info_buffer("level_seed");
    register_info_buffer("level_complete");
    register_obs_buffer("rgb");

}

Game::~Game() {
}

void Game::parse_options(std::string name, VecOptions opts) {
    opts.consume_bool("use_easy_jump", &options.use_easy_jump);
    opts.consume_bool("paint_vel_info", &options.paint_vel_info);
    opts.consume_bool("use_generated_assets", &options.use_generated_assets);
    opts.consume_bool("center_agent", &options.center_agent);
    opts.consume_bool("use_sequential_levels", &options.use_sequential_levels);

    int dist_mode = EasyMode;
    opts.consume_int("distribution_mode", &dist_mode);
    options.distribution_mode = static_cast<DistributionMode>(dist_mode);

    if (options.distribution_mode == EasyMode) {
        fassert(name != "coinrun_old");
    } else if (options.distribution_mode == HardMode) {
        // all environments support this mode
    } else if (options.distribution_mode == ExtremeMode) {
        fassert(name == "chaser" || name == "dodgeball" || name == "leaper" || name == "starpilot");
    } else if (options.distribution_mode == MemoryMode) {
        fassert(name == "collector" || name == "caveflyer" || name == "dodgeball" || name == "heist" || name == "jumper" || name == "maze" || name == "miner");
    } else {
        fatal("invalid distribution_mode %d\n", options.distribution_mode);
    }

    // coinrun_old
    opts.consume_int("plain_assets", &options.plain_assets);
    opts.consume_int("physics_mode", &options.physics_mode);
    opts.consume_int("debug_mode", &options.debug_mode);
    opts.consume_int("game_type", &game_type);


    for (auto name : opts.get_names()){
      if (options.exists<uint8_t>(name)){
        bool dummy;
        opts.consume_bool(name, &dummy);
        options.assign<uint8_t>(name, dummy);
      }else if (options.exists<int32_t>(name)){
        int dummy;
        opts.consume_int(name, &dummy);
        options.assign<int32_t>(name, dummy);
      }else if (options.exists<float>(name)){
        float dummy;
        opts.consume_float(name, &dummy);
        options.assign<float>(name, dummy);
      }
    }

    opts.ensure_empty();
}

void Game::render_to_buf(void *dst, int w, int h, bool antialias) {
    // Qt focuses on RGB32 performance:
    // https://doc.qt.io/qt-5/qpainter.html#performance
    // so render to an RGB32 buffer and then convert it rather than render to RGB888 directly
    QImage img((uchar *)dst, w, h, w * 4, QImage::Format_RGB32);
    QPainter p(&img);

    if (antialias) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    }

    QRect rect = QRect(0, 0, w, h);
    game_draw(p, rect);
}

void Game::reset() {
    reset_count++;

    if (episodes_remaining == 0) {
        if (options.use_sequential_levels && step_data.level_complete) {
            // prevent overflow in seed sequences
            current_level_seed = (int32_t)(current_level_seed + 997);
        } else {
            current_level_seed = level_seed_rand_gen.randint(level_seed_low, level_seed_high);
        }

        episodes_remaining = 1;
    } else {
        step_data.reward = 0;
        step_data.done = false;
        step_data.level_complete = false;
    }

    rand_gen.seed(current_level_seed);
    game_reset();

    cur_time = 0;
    total_reward = 0;
    episodes_remaining -= 1;
    action = default_action;
}

void Game::step() {
    cur_time += 1;
    bool will_force_reset = false;

    if (action == -1) {
        action = default_action;
        will_force_reset = true;
    }

    step_data.reward = 0;
    step_data.done = false;
    step_data.level_complete = false;
    game_step();

    step_data.done = step_data.done || will_force_reset || (cur_time >= timeout);
    total_reward += step_data.reward;

    if (step_data.reward != 0) {
        last_reward_timer = 10;
        last_reward = step_data.reward;
    }

    int level_seed = current_level_seed;

    if (step_data.done) {
        last_ep_reward = total_reward;
        reset();
    }

    if (options.use_sequential_levels && step_data.level_complete) {
        step_data.done = false;
    }

    episode_done = step_data.done;

    auto ptr = point_to_obs<uint8_t>("rgb");
    if (ptr != 0){
      render_to_buf(render_buf, RES_W, RES_H, false);
      bgr32_to_rgb888(ptr, render_buf, RES_W, RES_H);
    }

    *reward_ptr = step_data.reward;
    *done_ptr = (uint8_t)step_data.done;
    assign_to_info("level_seed",(int32_t)(level_seed));
    assign_to_info("level_complete",(uint8_t)(step_data.level_complete));
}

void Game::game_init() {
}

void Game::register_info_buffer(std::string name){
  info_buffers[name] = GameSpaceBuffer();
}

void Game::register_obs_buffer(std::string name){
  obs_buffers[name] = GameSpaceBuffer();
}

void Game::connect_buffer(std::map<std::string, GameSpaceBuffer> &buffer_map, const std::vector<struct libenv_space> &spaces, const std::vector<void *> &buffer){
  for (int i = 0 ; i < spaces.size(); i++){
    auto bptr = buffer_map.find(spaces[i].name);
    if (bptr == buffer_map.end()){
      printf("No in-game buffer registerd for space '%s'\n", spaces[i].name);
      fassert(false);
    }
    bptr->second.space = &spaces[i];
    bptr->second.buffer = buffer[i];
  }
}

void Game::connect_info_buffer(const std::vector<struct libenv_space> &spaces, const std::vector<void *> &buffer){
  connect_buffer(info_buffers, spaces, buffer);
}

void Game::assign_to_info(const std::string name, uint8_t value){
  assign_to_buffer<uint8_t>(name, info_buffers, value);
}

void Game::assign_to_info(const std::string name, int32_t value){
  assign_to_buffer<int32_t>(name, info_buffers, value);
}

void Game::assign_to_info(const std::string name, float value){
  assign_to_buffer<float>(name, info_buffers, value);
}


void Game::connect_obs_buffer(const std::vector<struct libenv_space> &spaces, const std::vector<void *> &buffer){
  connect_buffer(obs_buffers, spaces, buffer);
}

void Game::assign_to_obs(const std::string name, uint8_t value){
  assign_to_buffer<uint8_t>(name, obs_buffers, value);
}

void Game::assign_to_obs(const std::string name, int32_t value){
  assign_to_buffer<int32_t>(name, obs_buffers, value);
}

void Game::assign_to_obs(const std::string name, float value){
  assign_to_buffer<float>(name, obs_buffers, value);
}
