#include <iostream>
using namespace std;

int main(){
    int n,m;
    cin >> n >> m;
    long long room_i_dormitory[n];
    long long room_number[m];
    for(int i = 0;i<n;i++){
        cin >> room_i_dormitory[i];
    }
    for(int i = 0;i<m;i++){
        cin >> room_number[i];
    }
    for(int i = 0;i<n;i++){
        for(int j = 0;j<i;j++){
            room_i_dormitory[i]+=room_i_dormitory[j];
        }
    }

    for(int i = 0;i<m;i++){
        for(int j = 0;j<n;j++){
            if(j==0){
                if(room_number[i]<=room_i_dormitory[j]){
                    cout << j+1 << " " << room_number[i] << endl;
                    break;
                }
            }
            else{
                if(room_number[i]<=room_i_dormitory[j]){
                cout << j+1 << " " << room_number[i]-room_i_dormitory[j-1] << endl;
                break;
                }
            }
        }
    }  
}