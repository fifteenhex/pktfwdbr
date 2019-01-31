project('pktfwdbr', 'c')

mosquitto_glib = subproject('mosquitto_glib')

conf_data = configuration_data()
conf_data.set('max_forwarders', get_option('max_forwarders'))
configure_file(input : 'config.h.in',
               output : 'config.h',
               configuration : conf_data)

src = ['pktfwdbr.c']
deps = [ dependency('glib-2.0'),
         dependency('json-glib-1.0'), 
         mosquitto_glib.get_variable('mosquitto_glib_dep')]

executable('pktfwdbr', src, 
           dependencies : deps, 
           install : true,
           install_dir : 'sbin')