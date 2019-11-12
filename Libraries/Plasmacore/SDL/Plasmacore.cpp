#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

#include <cstdint>

#include "Plasmacore.h"
#include "PlasmacoreMessage.h"
#include "PlasmacoreView.h"
#include "RogueInterface.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdlib.h>
#else
#include <libgen.h>
#include <unistd.h>
#endif

//#define WINDOW_BASED 1

// The "code" value for SDL_USEREVENT for our async call mechanism
#define ASYNC_CALL_EVENT 1

static int gargc;
static char ** gargv;


static volatile bool plasmacore_launched = false;


#ifdef __EMSCRIPTEN__
static int iterations;

static void do_async_call ( void (*cb)(void *), int millis )
{
  emscripten_async_call(cb, 0, millis);
}



static EM_BOOL on_emscripten_display_size_changed( int event_type, const EmscriptenUiEvent *event, void *user_data )
{
  PlasmacoreView::display_size_changed = true;
  return false;
}
#else

static Uint32 sdl_async_cb_poster (Uint32 interval, void * arg)
{
  SDL_Event event;
  event.user.type = SDL_USEREVENT;
  event.user.code = ASYNC_CALL_EVENT;
  event.user.data1 = arg; // The callback
  //userevent.data2 = ...
  SDL_PushEvent(&event);
  return 0;
}

static void do_async_call ( void (*cb)(void *), int millis )
{
  SDL_AddTimer(millis, sdl_async_cb_poster, (void *)cb);
}

#endif


void Plasmacore::set_message_handler( const char * type, HandlerCallback handler )
{
  auto info = new PlasmacoreMessageHandler( type, handler );
  handlers[ type ] = info;
}


Plasmacore & Plasmacore::configure()
{
  if (is_configured) return *this;
  is_configured = true;

  set_message_handler( "", [] (PlasmacoreMessage* m)
    {
        auto entry = singleton.reply_handlers.find( m->message_id );
        if (entry)
        {
          auto handler = entry->value;
          singleton.reply_handlers.remove( m->message_id );
          handler->callback( m );
          delete handler;
        }
    }
  );

  #ifdef WINDOW_BASED
  set_message_handler( "Window.create", [] (PlasmacoreMessage m)
    {
      auto name = m.getString( "name" );

      auto view = plasmacore_new_view(name);
      if (!view) throw "No view created!";

      Plasmacore::singleton.resources[ m.getInt32("id") ] = view;
      //fprintf( stderr, "Controller window: " << view << std::endl;
    }
  );

  set_message_handler( "Window.show", [] (PlasmacoreMessage m)
    {
      auto window_id = m.getInt32( "id" );
      auto view  = (PlasmacoreView*)Plasmacore::singleton.resources[ window_id ];
      if (view) view->show();
    }
  );
  #else
  auto view = plasmacore_new_view("Main");
  if (!view) throw "No view created!";
  //std::cerr << "Controller window: " << view << std::endl;
  #endif

  emscripten_set_resize_callback( nullptr, nullptr, false, on_emscripten_display_size_changed );

  RogueInterface_set_arg_count( gargc );
  for (int i = 0; i < gargc; ++i)
  {
    RogueInterface_set_arg_value( i, gargv[i] );
  }

  RogueInterface_configure();
  return *this;
}


RID Plasmacore::getResourceID( void * resource)
{
  if (! resource) return 0;

  for (auto const & entry : resources)
  {
    if (entry->value == resource) { return entry->key; }
  }
  return 0;
}


Plasmacore & Plasmacore::launch()
{
  if (is_launched) { return *this; }
  is_launched = true;

  RogueInterface_launch();
  auto m = PlasmacoreMessage( "Application.on_launch" );
#ifdef WINDOW_BASED
  m.set( "is_window_based", true );
#endif
  m.post();
  return *this;
}




Plasmacore & Plasmacore::relaunch()
{
  //XXX: Always relaunch window based?
  PlasmacoreMessage( "Application.on_launch" ).set( "is_window_based", true ).send();
  return *this;
}


void Plasmacore::remove_message_handler( const char* type )
{
  if (handlers.contains(type))
  {
    auto handler = handlers[ type ];
    handlers.remove( type );
    delete handler;
  }
}


void Plasmacore::post( PlasmacoreMessage & m )
{
  auto size = m.data.count;
  pending_message_data.add( uint8_t((size>>24)&255) );
  pending_message_data.add( uint8_t((size>>16)&255) );
  pending_message_data.add( uint8_t((size>>8)&255) );
  pending_message_data.add( uint8_t(size&255) );
  for (int i = 0; i < m.data.count; ++i)
  {
    pending_message_data.add( m.data[i] );
  }
  real_update(false);
}


void Plasmacore::post_rsvp( PlasmacoreMessage & m, HandlerCallback callback )
{
  reply_handlers[ m.message_id ] = new PlasmacoreMessageHandler( "", callback );
  post( m );
}


Plasmacore & Plasmacore::setIdleUpdateFrequency( double f )
{
  idleUpdateFrequency = f;
  if (update_timer)
  {
    stop();
    start();
  }
  return *this;
}


void Plasmacore::start()
{
  if ( !is_launched ) { configure().launch(); }
  printf("LOG: start()\n");

  update_timer = true;
  real_update(true);
}


void Plasmacore::stop()
{
  // Note that we don't actually stop any timer from running.  The major
  // outcome here is that you may get an extra update if you stop, restart,
  // or change the update frequency.
  // We *could* probably make this work "right" by using a cancellable SDL
  // timer, but I don't think it really matters, and I'm not sure that such
  // timers are available in emscripten, so we'd have to do this for emscripten
  // mode anyway.
  update_timer = false;
}


void Plasmacore::update(void * dummy)
{
  //printf("LOG: update(); iterations=%i\n", iterations);
  Plasmacore::singleton.real_update(true);
}

void Plasmacore::fast_update(void * dummy)
{
  //printf("LOG: update(); iterations=%i\n", iterations);
  Plasmacore::singleton.real_update(false);
}

void Plasmacore::real_update (bool reschedule)
{
  if (!Plasmacore::singleton.update_timer) return; // Ignore, since timer shouldn't be running.

  // Schedule the next idle update.
  if (reschedule) do_async_call(update, int(1000*idleUpdateFrequency));

  if (is_sending)
  {
    update_requested = true;
    return;
  }

  is_sending = true;

  // Execute a small burst of message dispatching and receiving.  Stop after
  // 10 iterations or when there are no new messages.  Global state updates
  // are frequency capped to 1/60 second intervals and draws are synced to
  // the display refresh so this isn't triggering large amounts of extra work.
  for (int _ = 0; _ < 10; ++_)
  {
    update_requested = false;

    // Swap pending data with io_buffer data
    io_buffer.clear().add( pending_message_data );
    pending_message_data.clear();

    RogueInterface_post_messages( io_buffer );
    auto count = io_buffer.count;
    //if (count || io_buffer.size())
    //  printf("TX:%-8lu  RX:%-8lu  @iter:%i\n", io_buffer.size(), count, iterations);
    uint8_t * bytes = &io_buffer[0];

    int read_pos = 0;
    while (read_pos+4 <= count)
    {
      auto size = int( bytes[read_pos] ) << 24;
      size |= int( bytes[read_pos+1] ) << 16;
      size |= int( bytes[read_pos+2] ) << 8;
      size |= int( bytes[read_pos+3] );
      read_pos += 4;

      if (read_pos + size <= count)
      {
        decode_buffer.clear().reserve( size );
        for (int i = 0; i < size; ++i)
        {
          decode_buffer.add( bytes[read_pos+i] );
        }

        auto m = PlasmacoreMessage( decode_buffer );
        dispatch( m );
        if (m._reply && !m._reply->sent)
        {
          m._reply->post();
        }
      }
      else
      {
        fprintf( stderr, "*** Skipping message due to invalid size.\n" );
      }
      read_pos += size;
    }

    io_buffer.clear();

    if ( !update_requested )
    {
      break;
    }
  }

  is_sending = false;

  if (update_requested)
  {
    // There are still some pending messages after 10 iterations.  Schedule another round
    // in 1/60 second instead of the usual 1.0 seconds.
    // We'll now have another (slow) call to update, but it'll probably return
    do_async_call(fast_update, 16);
  }
}

void Plasmacore::dispatch( PlasmacoreMessage& m )
{
  auto entry = handlers.find( m.type.characters );
  if (entry)
  {
    entry->value->callback( &m );
  }
}


static bool should_quit = false;

static int current_fps = 0;

#ifdef __EMSCRIPTEN__
static int new_fps = 60;
#else
static int new_fps = 60;
#endif

#include <sys/time.h>

#ifdef __EMSCRIPTEN__
static unsigned int start_time = 0;

static unsigned int get_ticks (void)
{
  struct timeval nowts;
  gettimeofday(&nowts, NULL);
  unsigned int now = nowts.tv_sec * 1000000 + nowts.tv_usec;
  return now;
}
#endif

static void do_iteration (void)
{
  if ( !plasmacore_launched ) return;

#ifdef __EMSCRIPTEN__
  if (current_fps != new_fps)
  {
    printf("Changing FPS from %i to %i\n", current_fps, new_fps);
    current_fps = new_fps;
    start_time = get_ticks();
    iterations = 0;
  }

  unsigned int now = get_ticks();
  unsigned int delta = now - start_time;
  int should = (int)(delta / 1000000.0 * current_fps);
  if (should > 0x0fffffff)
  {
    start_time = now;
    iterations = 0;
  }
  //printf("%i should; %i have\n", should, iterations);
  if (should <= iterations) return;
  ++iterations;
  //if (iterations % 5 != 0) return;
#endif

  plasmacore_redraw_all_windows();

#ifdef __EMSCRIPTEN__
  PlasmacoreView::display_size_changed = false;
#endif

  SDL_Event e;
  while (SDL_PollEvent(&e))
  {
    switch (e.type)
    {
      case SDL_QUIT:
        should_quit = true;
        return;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
      {
        auto w = plasmacore_get_window(e.button.windowID);
        if (!w) break;
        int which;
        switch (e.button.button)
        {
          case SDL_BUTTON_LEFT:
            which = 0;
            break;
          case SDL_BUTTON_RIGHT:
            which = 1;
            break;
          default:
            return;
        }
        if (e.type == SDL_MOUSEBUTTONDOWN)
          w->on_mouse_down(e.button.x, e.button.y, which);
        else
          w->on_mouse_up(e.button.x, e.button.y, which);
        break;
      }
      case SDL_MOUSEMOTION:
      {
        auto w = plasmacore_get_window(e.motion.windowID);
        w->on_mouse_move(e.motion.x, e.motion.y);
        break;
      }
      case SDL_FINGERMOTION:
      {
        auto m = PlasmacoreMessage( "Input.on_stylus_event" );
        m.set( "type", 0 );  // 0=move
        m.set( "x", e.tfinger.x );
        m.set( "y", e.tfinger.y );
        m.post();
        break;
      }
      case SDL_FINGERDOWN:
      {
        auto m = PlasmacoreMessage( "Input.on_stylus_event" );
        m.set( "type", 1 );  // 1=press
        m.set( "x", e.tfinger.x );
        m.set( "y", e.tfinger.y );
        m.post();
        break;
      }
      case SDL_FINGERUP:
      {
        auto m = PlasmacoreMessage( "Input.on_stylus_event" );
        m.set( "type", 2 );  // 2=release
        m.set( "x", e.tfinger.x );
        m.set( "y", e.tfinger.y );
        m.post();
        break;
      }
      case SDL_WINDOWEVENT:
      {
        auto w = plasmacore_get_window(e.button.windowID);
        if (!w) break;
        switch (e.window.event)
        {
          case SDL_WINDOWEVENT_FOCUS_GAINED:
            w->on_focus_gained();
            break;
        }
        break;
      }
      case SDL_USEREVENT:
      {
        if (e.user.code == ASYNC_CALL_EVENT)
        {
          // The following cast is theoretically not allowed.  Let's hope
          // it works anyway on platforms we care about.
          void (*f) (void*) = (void (*)(void*))e.user.data1;
          f(0);
          break;
        }
      }
    }
  }
}

Plasmacore Plasmacore::singleton;

extern "C" void Rogue_sync_local_storage()
{
  #ifdef __EMSCRIPTEN__
  EM_ASM(
     FS.syncfs( false, function (err) {
       Module.print("Synching IDBFS");
     });
  );
  #endif
}

extern "C" void launch_plasmacore()
{
  Plasmacore::singleton.configure().launch();
  PlasmacoreMessage( "Application.on_start" ).post();
  Plasmacore::singleton.start();

  plasmacore_launched = true;
}


extern "C" void Rogue_set_framerate (int fps)
{
  new_fps = fps;
  if (fps > current_fps) do_iteration();
}

extern "C" int Rogue_get_framerate (void)
{
  return current_fps;
}


//=============================================================================
//  PlasmacoreLauncher
//=============================================================================
PlasmacoreLauncher::PlasmacoreLauncher( int argc, char* argv[] ) : default_window_title("Plasmacore"), default_display_width(1024), default_display_height(768)
{
}

PlasmacoreLauncher::PlasmacoreLauncher( int argc, char* argv[], PlasmacoreCString default_window_title, int default_display_width, int default_display_height )
  : argc(argc), argv(argv), default_window_title(default_window_title), default_display_width(default_display_width), default_display_height(default_display_height)
{
}

int PlasmacoreLauncher::launch()
{
  gargc = argc;
  gargv = argv;

  PlasmacoreView::default_window_title = default_window_title.characters;
  PlasmacoreView::default_display_width = default_display_width;
  PlasmacoreView::default_display_height = default_display_height;

  #ifdef __EMSCRIPTEN__
    auto flags = 0;
  #else
    auto flags = SDL_INIT_TIMER;

    // CD into what we think the executable's directory is.
    char * exe = strdup(argv[0]);
    char * dir = dirname(exe);

    // chdir complains on unused result. 0 is success.
    if (chdir(dir)) free( exe );
    else            free( exe );
  #endif

  if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|flags) != 0) return 1;

  // Might want to call this here... but it might also be easier to just
  // call it from Rogue via "native".
  //SDL_ShowCursor(false);

#ifdef __EMSCRIPTEN__
  #ifdef LOCAL_FS
    plasmacore_launched = false;
    EM_ASM_({
       var mountpoint = Module["UTF8ToString"]($0);
       FS.mkdir(mountpoint);
       FS.mount(IDBFS, {}, mountpoint);
       FS.syncfs(true, function (err) {
         Module.print("IDBFS ready");
         Module["_launch_plasmacore"]();
       });
    }, LOCAL_FS);
  #else
    launch_plasmacore();
  #endif
  emscripten_set_main_loop(do_iteration, 0, 1);
#else
  launch_plasmacore();

  SDL_GL_SetSwapInterval(1);
  while (!should_quit)
  {
    do_iteration();
    //SDL_Delay(20);
  }
#endif

  return 0; // no error
}

