mcpx_ss.add(sdl, files(
	'apu.c',
	'debug.c',
	'monitor.c',
	))

if is_uwp
	mcpx_ss.add(cc.find_library('xaudio2_9', required: true))
endif

subdir('vp')
subdir('dsp')
