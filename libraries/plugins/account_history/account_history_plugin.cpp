#include <steem/plugins/account_history/account_history_plugin.hpp>

#include <steem/chain/util/impacted.hpp>

#include <steem/protocol/config.hpp>

#include <steem/chain/operation_notification.hpp>
#include <steem/chain/account_object.hpp>
#include <steem/chain/history_object.hpp>

#include <steem/utilities/plugin_utilities.hpp>

#include <fc/io/json.hpp>
#include <fc/smart_ref_impl.hpp>

#include <boost/algorithm/string.hpp>


#define STEEM_NAMESPACE_PREFIX "steem::protocol::"

namespace steem { namespace plugins { namespace account_history {

using namespace steem::protocol;

using chain::database;
using chain::operation_notification;
using chain::operation_id_type;
using chain::operation_object;
using chain::account_object;
using chain::account_history_object;

namespace detail {

enum
   {
      OPERATION_HISTORY_MAX_LENGTH = 30
   };

class account_history_plugin_impl
{
   public:
      account_history_plugin_impl() :
         _db( appbase::app().get_plugin< steem::plugins::chain::chain_plugin >().db() ) {}

      virtual ~account_history_plugin_impl() {}

      void on_operation( const operation_notification& note );

      flat_map< account_name_type, account_name_type > _tracked_accounts;
      bool                                             _filter_content = false;
      bool                                             _blacklist = false;
      flat_set< string >                               _op_list;
      bool                                             _prune = true;
      database&                        _db;
      boost::signals2::connection      pre_apply_connection;
};

struct operation_visitor
{
   operation_visitor( database& db, const operation_notification& note, const operation_object*& n,
      const account_object& account, bool prune )
      :_db(db), _note(note), new_obj(n), _account(account), _prune(prune) {}

   typedef void result_type;

   database& _db;
   const operation_notification& _note;
   const operation_object*& new_obj;
   const account_object& _account;
   bool _prune;

   template<typename Op>
   void operator()( Op&& )const
   {
      /// \warning This visitor can be called multiple times (in given op. context) for separate accounts
      if( !new_obj )
      {
         new_obj = &_db.create<operation_object>( [&]( operation_object& obj )
         {
            obj.trx_id       = _note.trx_id;
            obj.block        = _note.block;
            obj.trx_in_block = _note.trx_in_block;
            obj.timestamp    = _db.head_block_time();
            //fc::raw::pack( obj.serialized_op , _note.op);  //call to 'pack' is ambiguous
            auto size = fc::raw::pack_size( _note.op );
            obj.serialized_op.resize( size );
            fc::datastream< char* > ds( obj.serialized_op.data(), size );
            fc::raw::pack( ds, _note.op );
         });
      }

      const auto& op_idx = _db.get_index<chain::operation_index, chain::by_id>();
      const auto& hist_idx = _db.get_index<chain::account_history_index, chain::by_account>();

      auto hist_itr = hist_idx.find(_account.id);

      auto now = _db.head_block_time();

      auto outdated_op_filter = [&now, &op_idx](const operation_id_type& opId) -> bool
         {
            auto opI = op_idx.find(opId);
            assert(opI != op_idx.end());
            const operation_object& op = *opI;
            return (now - op.timestamp) > fc::days(30);
         };

      if( hist_itr == hist_idx.end())
      {
         _db.create<account_history_object>(
            [this, &outdated_op_filter](account_history_object& o)
         {
            o.account  = _account.id;
            if(_prune == false || outdated_op_filter(new_obj->id) == false)
               o.store_operation(*new_obj);
         });
      }
      else
      {
         const account_history_object& histInfo = *hist_itr;
         _db.modify(histInfo,
            [this, &outdated_op_filter](account_history_object& o)
            {
               if( _prune )
               {
                  // Clean up accounts to last 30 days or 30 items, whichever is more.
                  o.truncate_operation_list(OPERATION_HISTORY_MAX_LENGTH - 1); /// new item will be added in a sec
                  auto filter(outdated_op_filter);
                  o.remove_outdated_operations(std::move(filter));
                  if(outdated_op_filter(new_obj->id) == false)
                     o.store_operation(*new_obj);
               }
               else
               {
                  o.store_operation(*new_obj);
               }
            }
         );
      }
   }
};

struct operation_visitor_filter : operation_visitor
{
   operation_visitor_filter( database& db, const operation_notification& note, const operation_object*& n,
      const account_object& account, const flat_set< string >& filter, bool prune, bool blacklist ):
      operation_visitor( db, note, n, account, prune ), _filter( filter ), _blacklist( blacklist ) {}

   const flat_set< string >& _filter;
   bool _blacklist;

   template< typename T >
   void operator()( const T& op )const
   {
      if( _filter.find( fc::get_typename< T >::name() ) != _filter.end() )
      {
         if( !_blacklist )
            operation_visitor::operator()( op );
      }
      else
      {
         if( _blacklist )
            operation_visitor::operator()( op );
      }
   }
};

void account_history_plugin_impl::on_operation( const operation_notification& note )
{
   flat_set<account_name_type> impacted;

   const operation_object* new_obj = nullptr;
   app::operation_get_impacted_accounts( note.op, impacted );

   for( const auto& item : impacted ) {
      auto itr = _tracked_accounts.lower_bound( item );

      /*
       * The map containing the ranges uses the key as the lower bound and the value as the upper bound.
       * Because of this, if a value exists with the range (key, value], then calling lower_bound on
       * the map will return the key of the next pair. Under normal circumstances of those ranges not
       * intersecting, the value we are looking for will not be present in range that is returned via
       * lower_bound.
       *
       * Consider the following example using ranges ["a","c"], ["g","i"]
       * If we are looking for "bob", it should be tracked because it is in the lower bound.
       * However, lower_bound( "bob" ) returns an iterator to ["g","i"]. So we need to decrement the iterator
       * to get the correct range.
       *
       * If we are looking for "g", lower_bound( "g" ) will return ["g","i"], so we need to make sure we don't
       * decrement.
       *
       * If the iterator points to the end, we should check the previous (equivalent to rbegin)
       *
       * And finally if the iterator is at the beginning, we should not decrement it for obvious reasons
       */
      if( itr != _tracked_accounts.begin() &&
          ( ( itr != _tracked_accounts.end() && itr->first != item  ) || itr == _tracked_accounts.end() ) )
      {
         --itr;
      }

      if( !_tracked_accounts.size() || (itr != _tracked_accounts.end() && itr->first <= item && item <= itr->second ) )
      {
         const account_object* account = _db.find_account(item);

         if(account != nullptr)
         {
         if(_filter_content)
            note.op.visit( operation_visitor_filter( _db, note, new_obj, *account, _op_list, _prune, _blacklist ) );
         else
            note.op.visit( operation_visitor( _db, note, new_obj, *account, _prune ) );
         }
      }
   }
}

} // detail

account_history_plugin::account_history_plugin() {}
account_history_plugin::~account_history_plugin() {}

void account_history_plugin::set_program_options(
   options_description& cli,
   options_description& cfg
   )
{
   cfg.add_options()
         ("account-history-track-account-range", boost::program_options::value< vector< string > >()->composing()->multitoken(), "Defines a range of accounts to track as a json pair [\"from\",\"to\"] [from,to] Can be specified multiple times.")
         ("track-account-range", boost::program_options::value< vector< string > >()->composing()->multitoken(), "Defines a range of accounts to track as a json pair [\"from\",\"to\"] [from,to] Can be specified multiple times. Deprecated in favor of account-history-track-account-range.")
         ("account-history-whitelist-ops", boost::program_options::value< vector< string > >()->composing(), "Defines a list of operations which will be explicitly logged.")
         ("history-whitelist-ops", boost::program_options::value< vector< string > >()->composing(), "Defines a list of operations which will be explicitly logged. Deprecated in favor of account-history-whitelist-ops.")
         ("account-history-blacklist-ops", boost::program_options::value< vector< string > >()->composing(), "Defines a list of operations which will be explicitly ignored.")
         ("history-blacklist-ops", boost::program_options::value< vector< string > >()->composing(), "Defines a list of operations which will be explicitly ignored. Deprecated in favor of account-history-blacklist-ops.")
         ("history-disable-pruning", boost::program_options::value< bool >()->default_value( false ), "Disables automatic account history trimming" )
         ;
}

void account_history_plugin::plugin_initialize( const boost::program_options::variables_map& options )
{
   my = std::make_unique< detail::account_history_plugin_impl >();

   my->pre_apply_connection = my->_db.pre_apply_operation.connect( 0, [&]( const operation_notification& note ){ my->on_operation(note); } );

   typedef pair< account_name_type, account_name_type > pairstring;
   STEEM_LOAD_VALUE_SET(options, "account-history-track-account-range", my->_tracked_accounts, pairstring);

   if( options.count( "track-account-range" ) )
   {
      wlog( "track-account-range is deprecated in favor of account-history-track-account-range" );
      STEEM_LOAD_VALUE_SET( options, "track-account-range", my->_tracked_accounts, pairstring );
   }


   if( options.count( "account-history-whitelist-ops" ) || options.count( "history-whitelist-ops" ) )
   {
      my->_filter_content = true;
      my->_blacklist = false;

      if( options.count( "account-history-whitelist-ops" ) )
      {
         for( auto& arg : options.at( "account-history-whitelist-ops" ).as< vector< string > >() )
         {
            vector< string > ops;
            boost::split( ops, arg, boost::is_any_of( " \t," ) );

            for( const string& op : ops )
            {
               if( op.size() )
                  my->_op_list.insert( STEEM_NAMESPACE_PREFIX + op );
            }
         }
      }

      if( options.count( "history-whitelist-ops" ) )
      {
         wlog( "history-whitelist-ops is deprecated in favor of account-history-whitelist-ops." );

         for( auto& arg : options.at( "history-whitelist-ops" ).as< vector< string > >() )
         {
            vector< string > ops;
            boost::split( ops, arg, boost::is_any_of( " \t," ) );

            for( const string& op : ops )
            {
               if( op.size() )
                  my->_op_list.insert( STEEM_NAMESPACE_PREFIX + op );
            }
         }
      }

      ilog( "Account History: whitelisting ops ${o}", ("o", my->_op_list) );
   }
   else if( options.count( "account-history-blacklist-ops" ) || options.count( "history-blacklist-ops" ) )
   {
      my->_filter_content = true;
      my->_blacklist = true;

      if( options.count( "account-history-blacklist-ops" ) )
      {
         for( auto& arg : options.at( "account-history-blacklist-ops" ).as< vector< string > >() )
         {
            vector< string > ops;
            boost::split( ops, arg, boost::is_any_of( " \t," ) );

            for( const string& op : ops )
            {
               if( op.size() )
                  my->_op_list.insert( STEEM_NAMESPACE_PREFIX + op );
            }
         }
      }

      if( options.count( "history-blacklist-ops" ) )
      {
         wlog( "history-blacklist-ops is deprecated in favor of account-history-blacklist-ops." );

         for( auto& arg : options.at( "history-blacklist-ops" ).as< vector< string > >() )
         {
            vector< string > ops;
            boost::split( ops, arg, boost::is_any_of( " \t," ) );

            for( const string& op : ops )
            {
               if( op.size() )
                  my->_op_list.insert( STEEM_NAMESPACE_PREFIX + op );
            }
         }
      }

      ilog( "Account History: blacklisting ops ${o}", ("o", my->_op_list) );
   }

   if( options.count( "history-disable-pruning" ) )
   {
      my->_prune = !options[ "history-disable-pruning" ].as< bool >();
   }
}

void account_history_plugin::plugin_startup() {}

void account_history_plugin::plugin_shutdown()
{
   chain::util::disconnect_signal( my->pre_apply_connection );
}

flat_map< account_name_type, account_name_type > account_history_plugin::tracked_accounts() const
{
   return my->_tracked_accounts;
}

} } } // steem::plugins::account_history
