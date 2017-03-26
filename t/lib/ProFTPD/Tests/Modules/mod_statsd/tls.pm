package ProFTPD::Tests::Modules::mod_statsd::tls;

use lib qw(t/lib);
use base qw(ProFTPD::TestSuite::Child);
use strict;

use Cwd;
use File::Path qw(mkpath rmtree);
use File::Spec;
use IO::Handle;
use Socket;

use ProFTPD::TestSuite::FTP;
use ProFTPD::TestSuite::Utils qw(:auth :config :running :test :testsuite);
use ProFTPD::Tests::Modules::mod_statsd::mgmt qw(:admin);

$| = 1;

# NOTE: Use the TEST_DEBUG environment variable for debugging the on-the-wire
# communication with the statsd management port.

my $order = 0;

my $TESTS = {
  statsd_tls_handshakes => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  statsd_tls_protocol_and_cipher => {
    order => ++$order,
    test_class => [qw(forking)],
  },

};

sub new {
  return shift()->SUPER::new(@_);
}

sub list_tests {
  # Check for the required Perl modules:
  #
  #  IO-Socket-SSL
  #  JSON
  #  Net-FTPSSL

  my $required = [qw(
    IO::Socket::SSL
    JSON
    Net::FTPSSL
  )];

  foreach my $req (@$required) {
    eval "use $req";
    if ($@) {
      print STDERR "\nWARNING:\n + Module '$req' not found, skipping all tests\n";

      if ($ENV{TEST_VERBOSE}) {
        print STDERR "Unable to load $req: $@\n";
      }

      return qw(testsuite_empty_test);
    }
  }

  # Check for required environment variables, pointing us at the local
  # statsd instance and our config files.

  $required = [qw(
    PROFTPD_TEST_LIB
    STATSD_MGMT_PORT
  )];

  $ENV{STATSD_PORT} = 8125 unless defined($ENV{STATSD_PORT});
  $ENV{STATSD_MGMT_PORT} = 8126 unless defined($ENV{STATSD_MGMT_PORT});

  foreach my $req (@$required) {
    unless (defined($ENV{$req})) {
      print STDERR "\nWARNING:\n + Environment variable '$req' not found, skipping all tests\n";
      return qw(testsuite_empty_test);
    }
  }

  return testsuite_get_runnable_tests($TESTS);
}

sub statsd_tls_handshakes {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'statsd');

  my $cert_file = File::Spec->rel2abs("$ENV{PROFTPD_TEST_LIB}/t/etc/modules/mod_tls/server-cert.pem");
  my $statsd_port = $ENV{STATSD_PORT};

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'statsd:20 statsd.statsd:20 statsd.metric:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdServer => "udp://127.0.0.1:$statsd_port",
      },

      'mod_tls.c' => {
        TLSEngine => 'on',
        TLSLog => $setup->{log_file},
        TLSRequired => 'on',
        TLSRSACertificateFile => $cert_file,
      }
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  delete_statsd_info();

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  require Net::FTPSSL;
  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      # Give the server a chance to start up
      sleep(1);

      my $client = Net::FTPSSL->new('127.0.0.1',
        Encryption => 'E',
        Port => $port,
      );

      unless ($client) {
        die("Can't connect to FTPS server: " . IO::Socket::SSL::errstr());
      }

      unless ($client->login($setup->{user}, $setup->{passwd})) {
        die("Can't login: " . $client->last_message());
      }

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $counters = get_statsd_info('counters');

    my $counter_names = [qw(
      tls.handshake.ctrl
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $self->assert($counts > 0,
        "Expected count values for $counter_name, found none");
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      tls.handshake.ctrl
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $self->assert(scalar(@$timings) > 0,
        "Expected timing values for $timer_name, found none");
    }
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub statsd_tls_protocol_and_cipher {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'statsd');

  my $cert_file = File::Spec->rel2abs("$ENV{PROFTPD_TEST_LIB}/t/etc/modules/mod_tls/server-cert.pem");
  my $statsd_port = $ENV{STATSD_PORT};

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'statsd:20 statsd.statsd:20 statsd.metric:20 tls:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdServer => "udp://127.0.0.1:$statsd_port",
      },

      'mod_tls.c' => {
        TLSEngine => 'on',
        TLSLog => $setup->{log_file},
        TLSRequired => 'on',
        TLSRSACertificateFile => $cert_file,
        TLSOptions => 'EnableDiags StdEnvVars',
        TLSProtocol => 'SSLv3 TLSv1 TLSv1.1',
      }
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  delete_statsd_info();

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  require Net::FTPSSL;
  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      # Give the server a chance to start up
      sleep(1);

      my $ssl_opts = {
        SSL_hostname => '127.0.0.1',
        SSL_version => 'SSLv3',
      };

      my $client = Net::FTPSSL->new('127.0.0.1',
        Encryption => 'E',
        Port => $port,
        SSL_Client_Certificate => $ssl_opts,
      );

      unless ($client) {
        die("Can't connect to FTPS server: " . IO::Socket::SSL::errstr());
      }

      unless ($client->login($setup->{user}, $setup->{passwd})) {
        die("Can't login: " . $client->last_message());
      }

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $counters = get_statsd_info('counters');

    my $counter_names = [qw(
      tls.handshake.ctrl
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $self->assert($counts > 0,
        "Expected count values for $counter_name, found none");
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      tls.handshake.ctrl
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $self->assert(scalar(@$timings) > 0,
        "Expected timing values for $timer_name, found none");
    }
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

1;
