package My::Suite::Galera;
use File::Basename;
use My::Find;

@ISA = qw(My::Suite);

return "Not run for embedded server" if $::opt_embedded_server;

return "WSREP is not compiled in" if not ::have_wsrep();

return "No wsrep provider library" unless ::have_wsrep_provider();

return ::wsrep_version_message() unless ::check_wsrep_version();

::suppress_galera_warings();

push @::global_suppressions,
   (
     qr(WSREP: Maximum writeset size exceeded by .*),
     qr(WSREP: transaction size exceeded.*),
     qr(WSREP: RBR event .*),
     qr(WSREP: Ignoring error for TO isolated action: .*),
     qr(WSREP: transaction size limit .*),
     qr(WSREP: rbr write fail, .*),
     qr(WSREP: .*Backend not supported: foo.*),
     qr(WSREP: .*Failed to initialize backend using .*),
     qr(WSREP: .*Failed to open channel 'my_wsrep_cluster' at .*),
     qr(WSREP: gcs connect failed: Socket type not supported),
     qr(WSREP: failed to open gcomm backend connection: 110: failed to reach primary view: 110 .*),
     qr(WSREP: .*Failed to open backend connection: -110 .*),
     qr(WSREP: .*Failed to open channel 'my_wsrep_cluster' at .*),
     qr(WSREP: gcs connect failed: Connection timed out),
     qr|WSREP: wsrep::connect\(.*\) failed: 7|,
     qr(WSREP: SYNC message from member .* in non-primary configuration. Ignored.),
     qr(WSREP: TO isolation failed for: .*),
     qr|WSREP: Unsupported protocol downgrade: incremental data collection disabled. Expect abort.|,
     qr(WSREP: discarding established .*),
     qr|WSREP: .*core_handle_uuid_msg.*|,
     qr|Query apply failed:*|,
     qr(WSREP: Ignoring error*),
     qr(WSREP: Failed to remove page file .*),
     qr(WSREP: wsrep_sst_method is set to 'mysqldump' yet mysqld bind_address is set to .*)
   );

bless { };
