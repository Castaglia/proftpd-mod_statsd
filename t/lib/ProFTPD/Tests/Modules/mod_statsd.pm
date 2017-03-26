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
use ProFTPD::Tests::Modules::mod_statsd::mgmt qw(:admin);

$| = 1;

# NOTE: Use the TEST_DEBUG environment variable for debugging the on-the-wire
# communication with the statsd management port.

my $order = 0;

my $TESTS = {
  statsd_engine => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  statsd_server_udp => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  # Note: this test requires that statsd be listening for TCP, not UDP.
  # Rather than disabling this test using the 'inprogress' tag, the
  # test itself should make a probe TCP connection, and simply return if
  # that TCP connection to statsd fails.
  statsd_server_tcp => {
    order => ++$order,
    test_class => [qw(forking inprogress)],
  },

  statsd_sampling => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  statsd_namespacing => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  statsd_timeout_login => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  statsd_exclude_filter => {
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

  $ENV{PROFTPD_TEST_DISABLE_CLASS} = 'inprogress';
  return testsuite_get_runnable_tests($TESTS);
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
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdServer => "127.0.0.1:$statsd_port",
      },
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

  eval {
    my $counters = get_statsd_info('counters');

    my $counter_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $self->assert($counts > 0,
        "Expected count values for $counter_name, found none");
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $self->assert(scalar(@$timings) > 0,
        "Expected timing values for $timer_name, found none");
    }

    my $gauges = get_statsd_info('gauges');

    # Our connection gauge is a GAUGE; we expect it to have the same value after
    # as before.
    $self->assert($gauges->{connection} == 0,
      "Expected connection gauge 0, got $gauges->{Connection}");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub statsd_server_udp {
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
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdServer => "udp://127.0.0.1:$statsd_port",
      },
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

  eval {
    my $counters = get_statsd_info('counters');

    my $counter_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $self->assert($counts > 0,
        "Expected count values for $counter_name, found none");
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $self->assert(scalar(@$timings) > 0,
        "Expected timing values for $timer_name, found none");
    }

    my $gauges = get_statsd_info('gauges');

    # Our connection gauge is a GAUGE; we expect it to have the same value after
    # as before.
    $self->assert($gauges->{connection} == 0,
      "Expected connection gauge 0, got $gauges->{connection}");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub statsd_server_tcp {
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
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdServer => "tcp://127.0.0.1:$statsd_port",
      },
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

  eval {
    my $counters = get_statsd_info('counters');

    my $counter_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $self->assert($counts > 0,
        "Expected count values for $counter_name, found none");
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $self->assert(scalar(@$timings) > 0,
        "Expected timing values for $timer_name, found none");
    }

    my $gauges = get_statsd_info('gauges');

    # Our connection gauge is a GAUGE; we expect it to have the same value after
    # as before.
    $self->assert($gauges->{connection} == 0,
      "Expected connection gauge 0, got $gauges->{connection}");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub statsd_sampling {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'statsd');

  my $statsd_port = 8125;
  my $sampling = 25.0;

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
        StatsdSampling => $sampling,
        StatsdServer => "udp://127.0.0.1:$statsd_port",
      },
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

  eval {
    my $counters = get_statsd_info('counters');

    my $counter_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      if ($ENV{TEST_VERBOSE}) {
        if ($counts > 0) {
          print STDERR "# Sampling $sampling: got $counter_name counter = $counts\n";
        }
      }
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      if ($ENV{TEST_VERBOSE}) {
        if ($timings &&
            scalar(@$timings) > 0) {
          print STDERR "# Sampling $sampling: got $timer_name timer\n";
        }
      }
    }

    my $gauges = get_statsd_info('gauges');

    if ($ENV{TEST_VERBOSE}) {
      if (defined($gauges->{connection})) {
        print STDERR "# Sampling $sampling: got connection gauge $gauges->{connection}\n";
      }
    }
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub statsd_namespacing {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'statsd');

  my $statsd_port = 8125;
  my $prefix = "proftpd.prod";
  my $suffix = "tests";

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
        StatsdServer => "udp://127.0.0.1:$statsd_port $prefix $suffix",
      },
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

  eval {
    my $counters = get_statsd_info('counters');

    my $counter_names = [
      "$prefix.command.USER.331.$suffix",
      "$prefix.command.PASS.230.$suffix",
      "$prefix.command.QUIT.221.$suffix"
    ];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $self->assert($counts > 0,
        "Expected count values for $counter_name, found none");
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [
      "$prefix.command.USER.331.$suffix",
      "$prefix.command.PASS.230.$suffix",
      "$prefix.command.QUIT.221.$suffix"
    ];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $self->assert(scalar(@$timings) > 0,
        "Expected timing values for $timer_name, found none");
    }

    my $gauges = get_statsd_info('gauges');

    # Our connection gauge is a GAUGE; we expect it to have the same value after
    # as before.
    my $gauge_name = "$prefix.connection.$suffix";
    $self->assert($gauges->{$gauge_name} == 0,
      "Expected $gauge_name gauge 0, got $gauges->{$gauge_name}");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub statsd_timeout_login {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'statsd');

  my $statsd_port = 8125;
  my $timeout_login = 3;

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'events:10 timer:20 statsd:20 statsd.statsd:20 statsd.metric:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    TimeoutLogin => $timeout_login,

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdServer => "udp://127.0.0.1:$statsd_port",
      },
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

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      if ($ENV{TEST_VERBOSE}) {
        print STDERR "# Waiting for $timeout_login secs\n";
      }
      sleep($timeout_login + 1);
      eval { $client->login($setup->{user}, $setup->{passwd}) };
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
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $counts == 0 unless $counts;

      $self->assert($counts == 0,
        "Expected count values for $counter_name, found none");
    }

    $self->assert($counters->{'timeout.TimeoutLogin'} == 1,
      "Expected count value for timeout.TimeoutLogin, found none");

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $timings = [] unless $timings;
      $self->assert(scalar(@$timings) == 0,
        "Expected no timing values for $timer_name, found some");
    }

    my $gauges = get_statsd_info('gauges');

    # Our connection gauge is a GAUGE; we expect it to have the same value after
    # as before.
    $self->assert($gauges->{connection} == 0,
      "Expected connection gauge 0, got $gauges->{connection}");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub statsd_exclude_filter {
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
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_statsd.c' => {
        StatsdEngine => 'on',
        StatsdExcludeFilter => '^SYST$',
        StatsdServer => "udp://127.0.0.1:$statsd_port",
      },
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

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});
      $client->syst();
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
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $counter_name (@$counter_names) {
      my $counts = $counters->{$counter_name};
      $self->assert($counts > 0,
        "Expected count values for $counter_name, found none");
    }

    my $timers = get_statsd_info('timers');

    # For timers, we simply expect to HAVE timings
    my $timer_names = [qw(
      command.USER.331
      command.PASS.230
      command.QUIT.221
    )];

    foreach my $timer_name (@$timer_names) {
      my $timings = $timers->{$timer_name};
      $self->assert(scalar(@$timings) > 0,
        "Expected timing values for $timer_name, found none");
    }

    my $gauges = get_statsd_info('gauges');

    # Our connection gauge is a GAUGE; we expect it to have the same value after
    # as before.
    $self->assert($gauges->{connection} == 0,
      "Expected connection gauge 0, got $gauges->{connection}");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

1;
