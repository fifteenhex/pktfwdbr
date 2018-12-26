project('pktfwdbr', 'c')

mosquitto_glib = subproject('mosquitto_glib')

src = ['pktfwdbr.c']
deps = [ dependency('glib-2.0'),
         dependency('json-glib-1.0'), 
         mosquitto_glib.get_variable('mosquitto_glib_dep')]

executable('pktfwdbr', src, 
           dependencies : deps, 
           install : true,
           install_dir : 'sbin')