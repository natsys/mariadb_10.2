package My::Suite::Galera_sr;
use File::Basename;
use My::Find;

@ISA = qw(My::Suite);

return "Not run for embedded server" if $::opt_embedded_server;

return "WSREP is not compiled in" if not ::have_wsrep();

return "No wsrep provider library" unless ::have_wsrep_provider();

return ::wsrep_version_message() unless ::check_wsrep_version();

::suppress_galera_warings();

bless { };
