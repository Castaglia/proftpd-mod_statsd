package ProFTPD::Tests::Modules::mod_statsd::mgmt;

use strict;

use IO::Handle;
use Socket;

require Exporter;
our @ISA = qw(Exporter);

our @ADMIN = qw(
  delete_statsd_info
  get_statsd_info
  statsd_mgmt
);

our @EXPORT_OK = (@ADMIN);

our %EXPORT_TAGS = (
  admin => [@ADMIN],
);

sub statsd_mgmt {
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

  return $client;
}

sub statsd_cmd {
  my $statsd = shift;
  my $cmd = shift;

  if ($ENV{TEST_DEBUG}) {
    print STDERR "# Sending command: $cmd\n";
  }

  $statsd->print("$cmd\n");
  $statsd->flush();

  my $resp = '';

  while (my $line = <$statsd>) {
    chomp($line);

    if ($ENV{TEST_DEBUG}) {
      print STDERR "# Received response: '$line'\n";
    }

    last if $line eq 'END';
    $resp .= $line;
  }

  return $resp;
}

sub delete_statsd_info {
  my $statsd = statsd_mgmt();

  my $cmd = "delcounters command.*";
  statsd_cmd($statsd, $cmd);

  $cmd = "deltimers command.*";
  statsd_cmd($statsd, $cmd);

  $cmd = "delgauges connections";
  statsd_cmd($statsd, $cmd);

  $statsd->close();
  return 1;
}

sub get_statsd_info {
  my $cmd = shift;

  my $statsd = statsd_mgmt();
  my $json = statsd_cmd($statsd, $cmd);
  $statsd->close();

  # statsd gives us (badly formatted) JSON; decode it into Perl.
  $json =~ s/ (\S+): / '\1': /g;
  $json =~ s/'{1,2}/\"/g;

  if ($ENV{TEST_JSON}) {
    print STDERR "# Received JSON: '$json'\n";
  }

  require JSON;
  JSON->import(qw(decode_json));

  my $info = decode_json($json);
  return $info;
}

1;
