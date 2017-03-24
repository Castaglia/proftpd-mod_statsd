package ProFTPD::Tests::Modules::mod_statsd;

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

$| = 1;

# NOTE: Use the TEST_DEBUG environment variable for debugging the on-the-wire
# communication with the statsd management port.

my $order = 0;

my $TESTS = {
  statsd_engine => {
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
  #  JSON

  my $required = [qw(
    JSON
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
  # statsd instance

  $required = [qw(
    STATSD_MGMT_PORT
  )];

  $ENV{STATSD_MGMT_PORT} = 8126 unless defined($ENV{STATSD_MGMT_PORT});

  foreach my $req (@$required) {
    unless (defined($ENV{$req})) {
      print STDERR "\nWARNING:\n + Environment variable '$req' not found, skipping all tests\n";
      return qw(testsuite_empty_test);
    }
  }

  return testsuite_get_runnable_tests($TESTS);
}

sub get_statsd_info {
  my $cmd = shift;

  my $port = $ENV{STATSD_MGMT_PORT};
  my $opts = {
    PeerHost => '127.0.0.1',
    PeerPort => $port,
    Proto => 'tcp',
    Type => SOCK_STREAM,
    Timeout => 3
  };

  my $client = IO::Socket::INET->new(%$opts);
  unless ($client) {
    croak("Can't connect to 127.0.0.1:$port: $!");
  }

  if ($ENV{TEST_DEBUG}) {
    print STDERR "# Sending command: $cmd\n";
  }

  $client->print("$cmd\n");
  $client->flush();

  my $json = '';

  while (my $line = <$client>) {
    chomp($line);

    if ($ENV{TEST_DEBUG}) {
      print STDERR "# Received response: '$line'\n";
    }

    last if $line eq 'END';
    $json .= $line;
  }

  $client->close();

  # statsd gives us (badly formatted) JSON; decode it into Perl.
  $json =~ s/ (\S+): / '\1': /g;
  $json =~ s/'{1,2}/\"/g;

  if ($ENV{TEST_JSON}) {
    print STDERR "# Received JSON: '$json'\n";
  }

  require JSON;
  my $info = decode_json($json);
  return $info;
}

sub statsd_engine {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'statsd');

  my $statsd_port = 8125;

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'statsd:20 statsd.statsd:20 statsd.metric:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},

    IfModules => {
      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdServer => "127.0.0.1:$statsd_port",
      },

      'mod_delay.c' => {
        DelayEngine => 'off',
      },
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  my $counters_before = get_statsd_info('counters');
  my $timers_before = get_statsd_info('timers');
  my $gauges_before = get_statsd_info('gauges');

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});
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

  my $counters_after = get_statsd_info('counters');

  my $counter_names = [qw(
    command.USER.331
    command.PASS.230
    command.QUIT.221
  )];

  foreach my $counter_name (@$counter_names) {
    my $count_before = $counters_before->{$counter_name};
    my $count_after = $counters_after->{$counter_name};

    my $counts = $count_after - $count_before;
    $self->assert($counts > 0,
      "Expected count values for $counter_name, found none");
  }

  my $timers_after = get_statsd_info('timers');

  # For timers, we simply expect to HAVE timings
  my $timer_names = [qw(
    command.USER.331
    command.PASS.230
    command.QUIT.221
  )];

  foreach my $timer_name (@$timer_names) {
    my $timings = $timers_after->{$timer_name};
    $self->assert(scalar(@$timings) > 0,
      "Expected timing values for $timer_name, found none");
  }

  my $gauges_after = get_statsd_info('gauges');

  # Our connection gauge is a GAUGE; we expect it to have the same value after
  # as before.
  $self->assert($gauges_after->{connection} == $gauges_before->{connection},
    "Expected connection gauge $gauges_before->{connection}, got $gauges_after->{Connection}");

  test_cleanup($setup->{log_file}, $ex);
}

1;
